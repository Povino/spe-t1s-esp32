#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_netif_glue.h"

#include "esp_eth_mac_lan865x.h"
#include "esp_eth_phy_lan865x.h"
#include "esp_eth_phy_lan86xx.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "T1S_LAN8651";

/*
 * ESP32 <-> Mikroe Two-Wire ETH Click wiring
 *
 * ESP32 GPIO18 -> Click SCK
 * ESP32 GPIO19 -> Click SDO / MISO
 * ESP32 GPIO23 -> Click SDI / MOSI
 * ESP32 GPIO25 -> Click CS
 * ESP32 GPIO22 -> Click IRQ / INT
 * RST is not connected
 * ESP32 3V3    -> Click 3.3V
 * ESP32 GND    -> Click GND
 */
#define PIN_NUM_MISO      19
#define PIN_NUM_MOSI      23
#define PIN_NUM_SCLK      18
#define PIN_NUM_CS        25
#define PIN_NUM_INT       22

#define ETH_SPI_HOST      SPI3_HOST
#define ETH_SPI_CLOCK_HZ  (1 * 1000 * 1000)

#define UDP_PORT          5005
#define UDP_BROADCAST_IP  "192.168.50.255"

#define APP_IP_LAST_OCTET (CONFIG_APP_NODE_ID + 1)

static volatile bool s_got_ip = false;

/**
 * @brief Stop the application after an unrecoverable configuration or runtime error.
 *
 * The firmware is intended for reproducible laboratory measurements. If a
 * configuration or initialization error is detected, the application remains
 * alive and periodically yields instead of continuing with invalid test state.
 */
static void stop_on_error(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Validate configuration values that depend on each other.
 *
 * Kconfig restricts individual option ranges, but relationships between
 * options still need runtime validation. In particular, the selected node ID
 * must be lower than the configured number of nodes in the tested topology.
 */
static void validate_config(void)
{
    if (CONFIG_APP_NODE_ID >= CONFIG_APP_NODE_COUNT) {
        ESP_LOGE(TAG, "Invalid configuration: APP_NODE_ID=%d, APP_NODE_COUNT=%d",
                 CONFIG_APP_NODE_ID, CONFIG_APP_NODE_COUNT);
        stop_on_error();
    }
}

/**
 * @brief Handle Ethernet link-state events from the ESP-IDF Ethernet driver.
 *
 * The handler logs Ethernet start/stop events and updates the global IP-ready
 * state when the link is disconnected or stopped. On link-up, it reads and
 * prints the active Ethernet MAC address.
 *
 * @param arg User context passed during event registration. Not used.
 * @param event_base Event base supplied by ESP-IDF. Expected to be ETH_EVENT.
 * @param event_id Ethernet event identifier.
 * @param event_data Event-specific data. For Ethernet events, this points to
 *                   the Ethernet driver handle.
 */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac_addr[6] = {0};
        esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

        ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr));

        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    }

    case ETHERNET_EVENT_DISCONNECTED:
        s_got_ip = false;
        ESP_LOGW(TAG, "Ethernet Link Down");
        break;

    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;

    case ETHERNET_EVENT_STOP:
        s_got_ip = false;
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;

    default:
        break;
    }
}

/**
 * @brief Handle the Ethernet got-IP event.
 *
 * The handler logs the assigned IPv4 configuration and marks the network
 * interface as ready for the UDP connectivity tasks.
 *
 * @param arg User context passed during event registration. Not used.
 * @param event_base Event base supplied by ESP-IDF. Expected to be IP_EVENT.
 * @param event_id IP event identifier.
 * @param event_data Event-specific data. Expected to point to ip_event_got_ip_t.
 */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP");
    ESP_LOGI(TAG, "IP:      " IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ip_info->gw));

    s_got_ip = true;
}

/**
 * @brief Configure a static IPv4 address for the Ethernet network interface.
 *
 * The DHCP client is stopped and the interface is assigned an address from
 * the 192.168.50.0/24 test subnet. The last octet is derived from the
 * configured node ID.
 *
 * @param eth_netif Ethernet network interface to configure.
 */
static void configure_static_ip(esp_netif_t *eth_netif)
{
    esp_err_t err = esp_netif_dhcpc_stop(eth_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_ERROR_CHECK(err);
    }

    esp_netif_ip_info_t ip_info = {0};

    ip_info.ip.addr      = ESP_IP4TOADDR(192, 168, 50, APP_IP_LAST_OCTET);
    ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    ip_info.gw.addr      = ESP_IP4TOADDR(192, 168, 50, 1);

    ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &ip_info));

    ESP_LOGI(TAG, "Static IP configured as 192.168.50.%d", APP_IP_LAST_OCTET);
}

/**
 * @brief Configure PLCA for the LAN8651 Ethernet interface.
 *
 * When PLCA is enabled, the function disables PLCA temporarily, sets the
 * configured PLCA node ID, sets the node count on the coordinator node
 * with ID 0, and enables PLCA again. When PLCA is disabled in menuconfig,
 * the function explicitly disables PLCA and leaves the PHY in CSMA/CD mode.
 *
 * @param eth_handle Ethernet driver handle used for LAN86xx PLCA ioctl calls.
 */
static void configure_plca(esp_eth_handle_t eth_handle)
{
#if CONFIG_APP_ENABLE_PLCA
    bool enable = false;
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, LAN86XX_ETH_CMD_S_EN_PLCA, &enable));

    uint8_t node_id = CONFIG_APP_NODE_ID;
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, LAN86XX_ETH_CMD_S_PLCA_ID, &node_id));

    if (node_id == 0) {
        uint8_t node_count = CONFIG_APP_NODE_COUNT;
        ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, LAN86XX_ETH_CMD_S_PLCA_NCNT, &node_count));
        ESP_LOGI(TAG, "PLCA coordinator enabled: node_id=%u, node_count=%u",
                 node_id, node_count);
    } else {
        ESP_LOGI(TAG, "PLCA normal node enabled: node_id=%u", node_id);
    }

    enable = true;
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, LAN86XX_ETH_CMD_S_EN_PLCA, &enable));
#else
    bool enable = false;
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, LAN86XX_ETH_CMD_S_EN_PLCA, &enable));
    ESP_LOGW(TAG, "PLCA disabled; using default CSMA/CD behavior");
#endif
}

/**
 * @brief Receive and log UDP broadcast messages from other nodes.
 *
 * The task waits until the Ethernet interface has an IP address, binds a UDP
 * socket to the configured test port, and continuously logs received broadcast
 * messages together with the sender address and port.
 *
 * @param arg FreeRTOS task argument. Not used.
 */
static void udp_rx_task(void *arg)
{
    (void)arg;

    while (!s_got_ip) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "RX socket creation failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        ESP_LOGW(TAG, "SO_REUSEADDR failed: errno=%d", errno);
    }

    struct sockaddr_in listen_addr = {0};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(UDP_PORT);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "RX bind failed: errno=%d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP RX listening on port %d", UDP_PORT);

    char rx_buffer[128];

    while (1) {
        struct sockaddr_in source_addr = {0};
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                           (struct sockaddr *)&source_addr, &socklen);

        if (len < 0) {
            ESP_LOGW(TAG, "recvfrom failed: errno=%d", errno);
            continue;
        }

        rx_buffer[len] = '\0';

        char addr_str[16];
        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str));

        ESP_LOGI(TAG, "RX from %s:%d -> %s",
                 addr_str, ntohs(source_addr.sin_port), rx_buffer);
    }
}

/**
 * @brief Periodically transmit UDP broadcast connectivity messages.
 *
 * The task waits until the Ethernet interface has an IP address, creates a UDP
 * broadcast socket, and periodically sends a short message containing the node
 * ID, static IP address, and sequence counter.
 *
 * @param arg FreeRTOS task argument. Not used.
 */
static void udp_tx_task(void *arg)
{
    (void)arg;

    while (!s_got_ip) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "TX socket creation failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        ESP_LOGE(TAG, "SO_BROADCAST failed: errno=%d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);
    dest_addr.sin_addr.s_addr = inet_addr(UDP_BROADCAST_IP);

    uint32_t seq = 0;
    char tx_buffer[128];

    ESP_LOGI(TAG, "UDP TX broadcasting to %s:%d", UDP_BROADCAST_IP, UDP_PORT);

    while (1) {
        int msg_len = snprintf(tx_buffer, sizeof(tx_buffer),
                               "node_id=%d ip=192.168.50.%d seq=%" PRIu32,
                               CONFIG_APP_NODE_ID, APP_IP_LAST_OCTET, seq++);

        if (msg_len < 0) {
            ESP_LOGW(TAG, "Failed to format UDP TX message");
            vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_UDP_INTERVAL_MS));
            continue;
        }

        if (msg_len >= (int)sizeof(tx_buffer)) {
            ESP_LOGW(TAG, "UDP TX message truncated");
            vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_UDP_INTERVAL_MS));
            continue;
        }

        int sent = sendto(sock, tx_buffer, (size_t)msg_len, 0,
                          (struct sockaddr *)&dest_addr, sizeof(dest_addr));

        if (sent < 0) {
            ESP_LOGW(TAG, "sendto failed: errno=%d", errno);
        } else if (sent != msg_len) {
            ESP_LOGW(TAG, "sendto sent partial datagram: sent=%d expected=%d",
                     sent, msg_len);
        } else {
            ESP_LOGI(TAG, "TX -> %s", tx_buffer);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_UDP_INTERVAL_MS));
    }
}

/**
 * @brief Application entry point for the LAN8651 10BASE-T1S connectivity test.
 *
 * The function initializes ESP-IDF networking, configures the static Ethernet
 * interface, initializes the SPI-connected LAN8651 MAC-PHY, applies PLCA
 * settings, attaches the Ethernet driver to esp-netif, registers event
 * handlers, starts UDP connectivity tasks, and starts the Ethernet driver.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "Starting LAN8651 10BASE-T1S bring-up");
    ESP_LOGI(TAG, "Node ID: %d", CONFIG_APP_NODE_ID);
    ESP_LOGI(TAG, "Static IP: 192.168.50.%d", APP_IP_LAST_OCTET);
    ESP_LOGI(TAG, "Configured PLCA node count: %d", CONFIG_APP_NODE_COUNT);
    ESP_LOGI(TAG, "SPI clock: %d Hz", ETH_SPI_CLOCK_HZ);

    validate_config();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    assert(eth_netif != NULL);

    configure_static_ip(eth_netif);

    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_err);
    }

    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = ETH_SPI_CLOCK_HZ,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 20,
    };

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    /*
     * The tested wiring leaves the Click board RST pin unconnected.
     * Reset control is therefore disabled in the PHY configuration.
     */
    phy_config.reset_gpio_num = -1;

    eth_lan865x_config_t lan865x_config =
        ETH_LAN865X_DEFAULT_CONFIG(ETH_SPI_HOST, &spi_devcfg);

    lan865x_config.int_gpio_num = PIN_NUM_INT;

    /*
     * The LAN8651 interrupt pin is used, so periodic polling is disabled.
     */
    lan865x_config.poll_period_ms = 0;

    esp_eth_mac_t *mac = esp_eth_mac_new_lan865x(&lan865x_config, &mac_config);
    assert(mac != NULL);

    esp_eth_phy_t *phy = esp_eth_phy_new_lan865x(&phy_config);
    assert(phy != NULL);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);

    esp_eth_handle_t eth_handle = NULL;
    esp_err_t eth_install_ret = esp_eth_driver_install(&eth_config, &eth_handle);
    if (eth_install_ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(eth_install_ret));
        ESP_LOGE(TAG, "LAN8651 not reachable over SPI.");
        ESP_LOGE(TAG, "Check wiring, 3.3 V power, GND, MISO/MOSI, CS, IRQ, and Q1/CS timing.");
        ESP_LOGE(TAG, "RST should remain unconnected; reset_gpio_num is configured as -1.");

        stop_on_error();
    }

    uint8_t mac_addr[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac_addr, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    ESP_LOGI(TAG, "Assigned Ethernet MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5]);

    configure_plca(eth_handle);

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    assert(glue != NULL);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               eth_event_handler, NULL));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               got_ip_event_handler, NULL));

    BaseType_t rx_task_created = xTaskCreate(udp_rx_task, "udp_rx_task",
                                             4096, NULL, 5, NULL);
    if (rx_task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create udp_rx_task");
        stop_on_error();
    }

    BaseType_t tx_task_created = xTaskCreate(udp_tx_task, "udp_tx_task",
                                             4096, NULL, 5, NULL);
    if (tx_task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create udp_tx_task");
        stop_on_error();
    }

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
}