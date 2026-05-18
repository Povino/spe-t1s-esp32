#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

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
#define PIN_NUM_MISO   19
#define PIN_NUM_MOSI   23
#define PIN_NUM_SCLK   18
#define PIN_NUM_CS     25
#define PIN_NUM_INT    22

#define ETH_SPI_HOST       SPI3_HOST
#define ETH_SPI_CLOCK_HZ   (24 * 1000 * 1000)

#define APP_IP_LAST_OCTET  (CONFIG_APP_NODE_ID + 1)
#define TEST_BROADCAST_IP  "192.168.50.255"
#define TEST_MAGIC         0x54315350U
#define TEST_VERSION       1
#define TEST_MAX_NODES     8
#define TEST_RX_BUFFER_SIZE 1500
#define TEST_RX_IDLE_TIMEOUT_US 3000000LL

#if CONFIG_APP_TEST_DEST_BROADCAST
#define TEST_DESTINATION_MODE_TEXT "broadcast"
#else
#define TEST_DESTINATION_MODE_TEXT "application-unicast"
#endif

static volatile bool s_got_ip = false;

/*
 * UDP payload used by the traffic-pattern test.
 * The full UDP payload is this header followed by
 * CONFIG_APP_TEST_PAYLOAD_BYTES bytes of generated test data.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;
    uint16_t payload_len;
    uint16_t destination_node_id;
    uint16_t reserved;
    uint32_t src_node_id;
    uint32_t seq;
    uint32_t tx_time_ms;
} test_udp_header_t;

typedef struct {
    bool seen;
    uint32_t packets;
    uint32_t first_seq;
    uint32_t last_seq;
    uint64_t udp_payload_bytes;
    uint64_t test_payload_bytes;
    int64_t first_rx_us;
    int64_t last_rx_us;
} rx_stat_t;

static rx_stat_t s_rx_stats[TEST_MAX_NODES];
static volatile bool s_rx_result_printed = false;

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
 * options still need runtime validation. The node ID, sender mask, destination
 * node, and packet size must match the configured topology and packet format.
 */
static void validate_config(void)
{
    if (CONFIG_APP_NODE_COUNT > TEST_MAX_NODES) {
        ESP_LOGE(TAG, "Invalid configuration: APP_NODE_COUNT=%d exceeds TEST_MAX_NODES=%d",
                 CONFIG_APP_NODE_COUNT, TEST_MAX_NODES);
        stop_on_error();
    }

    if (CONFIG_APP_NODE_ID >= CONFIG_APP_NODE_COUNT) {
        ESP_LOGE(TAG, "Invalid configuration: APP_NODE_ID=%d, APP_NODE_COUNT=%d",
                 CONFIG_APP_NODE_ID, CONFIG_APP_NODE_COUNT);
        stop_on_error();
    }

    uint32_t valid_node_mask = (1U << CONFIG_APP_NODE_COUNT) - 1U;
    uint32_t sender_mask = (uint32_t)CONFIG_APP_TEST_SENDER_MASK;

    if ((sender_mask & ~valid_node_mask) != 0) {
        ESP_LOGE(TAG, "Invalid configuration: APP_TEST_SENDER_MASK=%d references nodes outside APP_NODE_COUNT=%d",
                 CONFIG_APP_TEST_SENDER_MASK, CONFIG_APP_NODE_COUNT);
        stop_on_error();
    }

#if CONFIG_APP_TEST_DEST_UNICAST
    if (CONFIG_APP_TEST_DEST_NODE_ID >= CONFIG_APP_NODE_COUNT) {
        ESP_LOGE(TAG, "Invalid configuration: APP_TEST_DEST_NODE_ID=%d, APP_NODE_COUNT=%d",
                 CONFIG_APP_TEST_DEST_NODE_ID, CONFIG_APP_NODE_COUNT);
        stop_on_error();
    }
#endif

    const size_t packet_len = sizeof(test_udp_header_t) + CONFIG_APP_TEST_PAYLOAD_BYTES;

    if (packet_len > UINT16_MAX) {
        ESP_LOGE(TAG, "Invalid configuration: UDP packet length=%u exceeds uint16_t storage",
                 (unsigned int)packet_len);
        stop_on_error();
    }

    if (packet_len > TEST_RX_BUFFER_SIZE) {
        ESP_LOGE(TAG, "Invalid configuration: UDP packet length=%u exceeds RX buffer size=%d",
                 (unsigned int)packet_len, TEST_RX_BUFFER_SIZE);
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
 * interface as ready for the UDP traffic-pattern tasks.
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
 * @brief Check whether the current node is selected as a traffic sender.
 *
 * @return true when the current node ID is enabled in APP_TEST_SENDER_MASK,
 *         false otherwise.
 */
static bool is_local_node_sender(void)
{
    return (((uint32_t)CONFIG_APP_TEST_SENDER_MASK) & (1U << CONFIG_APP_NODE_ID)) != 0;
}

/**
 * @brief Build the UDP transport IPv4 address used by the traffic-pattern test.
 *
 * All traffic-pattern packets are sent as UDP broadcast frames to avoid ARP
 * resolution in the test firmware. In application-unicast mode, the selected
 * receiver is identified by destination_node_id in the measurement header and
 * non-destination nodes ignore the packet at application level.
 *
 * @param buffer Output buffer for the textual IPv4 address.
 * @param buffer_len Length of the output buffer.
 * @return true when the address was written successfully, false otherwise.
 */
static bool build_transport_ip(char *buffer, size_t buffer_len)
{
    int written = snprintf(buffer, buffer_len, "%s", TEST_BROADCAST_IP);
    return written > 0 && written < (int)buffer_len;
}

/**
 * @brief Print receiver-side traffic-pattern results when the test is complete.
 *
 * The function scans all per-source receive counters and prints one result
 * line for every source node that was observed. Packet loss is inferred from
 * the first and last sequence numbers and the number of received packets.
 * Results are printed only once, either when forced or after a period without
 * received packets.
 *
 * @param force When true, results are printed immediately if any source was
 *              observed. When false, results are printed only after a receive
 *              idle period.
 */
static void print_rx_results_if_ready(bool force)
{
    int64_t now_us = esp_timer_get_time();
    int64_t last_any_rx_us = 0;
    bool any_seen = false;

    for (int i = 0; i < TEST_MAX_NODES; i++) {
        if (s_rx_stats[i].seen) {
            any_seen = true;
            if (s_rx_stats[i].last_rx_us > last_any_rx_us) {
                last_any_rx_us = s_rx_stats[i].last_rx_us;
            }
        }
    }

    if (!any_seen || s_rx_result_printed) {
        return;
    }

    if (!force && (now_us - last_any_rx_us) < TEST_RX_IDLE_TIMEOUT_US) {
        return;
    }

    s_rx_result_printed = true;

    ESP_LOGI(TAG, "========== TRAFFIC RX RESULT local_node=%d ==========", CONFIG_APP_NODE_ID);

    for (int i = 0; i < TEST_MAX_NODES; i++) {
        rx_stat_t *st = &s_rx_stats[i];

        if (!st->seen) {
            continue;
        }

        uint32_t expected = 0;
        uint32_t lost = 0;

        if (st->last_seq >= st->first_seq) {
            expected = st->last_seq - st->first_seq + 1;
            if (expected >= st->packets) {
                lost = expected - st->packets;
            }
        }

        double duration_s = 0.0;
        if (st->last_rx_us > st->first_rx_us) {
            duration_s = (double)(st->last_rx_us - st->first_rx_us) / 1000000.0;
        }

        double udp_mbps = 0.0;
        double payload_mbps = 0.0;

        if (duration_s > 0.0) {
            udp_mbps = ((double)st->udp_payload_bytes * 8.0) / duration_s / 1000000.0;
            payload_mbps = ((double)st->test_payload_bytes * 8.0) / duration_s / 1000000.0;
        }

        double loss_pct = 0.0;
        if (expected > 0) {
            loss_pct = ((double)lost * 100.0) / (double)expected;
        }

        ESP_LOGI(TAG,
                 "RESULT_TRAFFIC_RX local_node=%d src_node=%d packets=%" PRIu32
                 " expected=%" PRIu32 " lost=%" PRIu32 " loss_pct=%.3f"
                 " udp_bytes=%" PRIu64 " payload_bytes=%" PRIu64
                 " duration_s=%.3f udp_mbps=%.3f payload_mbps=%.3f",
                 CONFIG_APP_NODE_ID,
                 i,
                 st->packets,
                 expected,
                 lost,
                 loss_pct,
                 st->udp_payload_bytes,
                 st->test_payload_bytes,
                 duration_s,
                 udp_mbps,
                 payload_mbps);
    }

    ESP_LOGI(TAG, "====================================================");
}

/**
 * @brief Receive UDP traffic-pattern packets and collect per-source statistics.
 *
 * The task waits until the Ethernet interface has an IP address, binds a UDP
 * socket to the configured test port, validates incoming traffic-pattern
 * headers, applies the application-level destination filter when enabled,
 * and updates packet, byte, sequence, loss, and timing counters for each
 * source node.
 *
 * @param arg FreeRTOS task argument. Not used.
 */
static void traffic_rx_task(void *arg)
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

    struct timeval timeout = {
        .tv_sec = 1,
        .tv_usec = 0,
    };

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGE(TAG, "SO_RCVTIMEO failed: errno=%d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in listen_addr = {0};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(CONFIG_APP_TEST_UDP_PORT);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "RX bind failed: errno=%d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Traffic RX listening on UDP port %d", CONFIG_APP_TEST_UDP_PORT);

    uint8_t rx_buf[TEST_RX_BUFFER_SIZE];

    while (1) {
        struct sockaddr_in source_addr = {0};
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                           (struct sockaddr *)&source_addr, &socklen);

        if (len < 0) {
            print_rx_results_if_ready(false);
            continue;
        }

        if (len < (int)sizeof(test_udp_header_t)) {
            continue;
        }

        test_udp_header_t hdr;
        memcpy(&hdr, rx_buf, sizeof(hdr));

        uint32_t magic = ntohl(hdr.magic);
        uint16_t version = ntohs(hdr.version);
        uint16_t header_len = ntohs(hdr.header_len);
        uint16_t payload_len = ntohs(hdr.payload_len);
        uint16_t destination_node_id = ntohs(hdr.destination_node_id);
        uint32_t src_node_id = ntohl(hdr.src_node_id);
        uint32_t seq = ntohl(hdr.seq);

        if (magic != TEST_MAGIC || version != TEST_VERSION) {
            continue;
        }

        if (header_len != sizeof(test_udp_header_t)) {
            continue;
        }

        if ((size_t)len != ((size_t)header_len + (size_t)payload_len)) {
            continue;
        }

        if (src_node_id >= CONFIG_APP_NODE_COUNT || src_node_id >= TEST_MAX_NODES) {
            continue;
        }

        if (src_node_id == CONFIG_APP_NODE_ID) {
            continue;
        }

#if CONFIG_APP_TEST_DEST_UNICAST
        if (destination_node_id != CONFIG_APP_NODE_ID) {
            continue;
        }
#else
        (void)destination_node_id;
#endif

        rx_stat_t *st = &s_rx_stats[src_node_id];
        int64_t now_us = esp_timer_get_time();

        if (!st->seen) {
            st->seen = true;
            st->first_seq = seq;
            st->first_rx_us = now_us;
        }

        st->packets++;
        st->last_seq = seq;
        st->last_rx_us = now_us;
        st->udp_payload_bytes += (uint64_t)len;
        st->test_payload_bytes += (uint64_t)payload_len;
    }
}

/**
 * @brief Transmit UDP traffic-pattern packets from selected sender nodes.
 *
 * Nodes not selected by APP_TEST_SENDER_MASK terminate this task and operate
 * as receivers only. Selected sender nodes wait for the configured start delay,
 * transmit UDP broadcast transport traffic for the configured duration,
 * and print final transmit counters and throughput values.
 *
 * @param arg FreeRTOS task argument. Not used.
 */
static void traffic_tx_task(void *arg)
{
    (void)arg;

    while (!s_got_ip) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!is_local_node_sender()) {
        ESP_LOGI(TAG, "This node is receiver only. Sender mask is %d",
                 CONFIG_APP_TEST_SENDER_MASK);
        vTaskDelete(NULL);
        return;
    }

    char transport_ip[16] = {0};
    if (!build_transport_ip(transport_ip, sizeof(transport_ip))) {
        ESP_LOGE(TAG, "Failed to build UDP transport IP address");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "This node is traffic-pattern sender");
    ESP_LOGI(TAG, "Destination mode: %s", TEST_DESTINATION_MODE_TEXT);
#if CONFIG_APP_TEST_DEST_UNICAST
    ESP_LOGI(TAG, "Application destination node ID: %d", CONFIG_APP_TEST_DEST_NODE_ID);
#endif
    ESP_LOGI(TAG, "UDP transport IP: %s", transport_ip);
    ESP_LOGI(TAG, "Test starts in %d seconds", CONFIG_APP_TEST_START_DELAY_S);

    vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_TEST_START_DELAY_S * 1000));

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
    dest_addr.sin_port = htons(CONFIG_APP_TEST_UDP_PORT);
    dest_addr.sin_addr.s_addr = inet_addr(transport_ip);

    const size_t packet_len = sizeof(test_udp_header_t) + CONFIG_APP_TEST_PAYLOAD_BYTES;

    uint8_t *tx_buf = calloc(1, packet_len);
    if (tx_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate TX buffer");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    test_udp_header_t *hdr = (test_udp_header_t *)tx_buf;
    uint8_t *payload = tx_buf + sizeof(test_udp_header_t);

    for (int i = 0; i < CONFIG_APP_TEST_PAYLOAD_BYTES; i++) {
        payload[i] = (uint8_t)(i & 0xff);
    }

    ESP_LOGI(TAG,
             "Starting UDP traffic-pattern test: sender=%d destination_mode=%s transport_ip=%s duration=%d s payload=%d B tx_delay=%d us",
             CONFIG_APP_NODE_ID,
             TEST_DESTINATION_MODE_TEXT,
             transport_ip,
             CONFIG_APP_TEST_DURATION_S,
             CONFIG_APP_TEST_PAYLOAD_BYTES,
             CONFIG_APP_TEST_TX_DELAY_US);

    uint32_t seq = 0;
    uint32_t tx_errors = 0;
    uint64_t tx_packets = 0;
    uint64_t tx_attempts = 0;
    uint64_t tx_udp_payload_bytes = 0;
    uint64_t tx_test_payload_bytes = 0;
    int last_send_errno = 0;

    int64_t start_us = esp_timer_get_time();
    int64_t end_us = start_us + ((int64_t)CONFIG_APP_TEST_DURATION_S * 1000000LL);

    while (esp_timer_get_time() < end_us) {
        hdr->magic = htonl(TEST_MAGIC);
        hdr->version = htons(TEST_VERSION);
        hdr->header_len = htons(sizeof(test_udp_header_t));
        hdr->payload_len = htons(CONFIG_APP_TEST_PAYLOAD_BYTES);
        hdr->destination_node_id = htons(CONFIG_APP_TEST_DEST_NODE_ID);
        hdr->reserved = 0;
        hdr->src_node_id = htonl(CONFIG_APP_NODE_ID);
        hdr->seq = htonl(seq);
        hdr->tx_time_ms = htonl((uint32_t)(esp_timer_get_time() / 1000));

        tx_attempts++;

        int sent = sendto(sock, tx_buf, packet_len, 0,
                        (struct sockaddr *)&dest_addr, sizeof(dest_addr));

        if (sent == (int)packet_len) {
            tx_packets++;
            tx_udp_payload_bytes += (uint64_t)sent;
            tx_test_payload_bytes += CONFIG_APP_TEST_PAYLOAD_BYTES;
            seq++;
        } else {
            tx_errors++;
            last_send_errno = errno;
            vTaskDelay(1);
        }

        if (CONFIG_APP_TEST_TX_DELAY_US > 0) {
            esp_rom_delay_us(CONFIG_APP_TEST_TX_DELAY_US);
        }

        if ((tx_attempts & 0x3f) == 0) {
            vTaskDelay(1);
        }
    }

    int64_t actual_end_us = esp_timer_get_time();
    double duration_s = (double)(actual_end_us - start_us) / 1000000.0;
    double udp_mbps = 0.0;
    double payload_mbps = 0.0;

    if (duration_s > 0.0) {
        udp_mbps = ((double)tx_udp_payload_bytes * 8.0) / duration_s / 1000000.0;
        payload_mbps = ((double)tx_test_payload_bytes * 8.0) / duration_s / 1000000.0;
    }

    ESP_LOGI(TAG, "========== TRAFFIC TX RESULT node=%d ==========", CONFIG_APP_NODE_ID);
    ESP_LOGI(TAG,
            "RESULT_TRAFFIC_TX node=%d destination_mode=%s transport_ip=%s packets=%" PRIu64
            " attempts=%" PRIu64 " errors=%" PRIu32 " last_errno=%d"
            " udp_bytes=%" PRIu64 " payload_bytes=%" PRIu64
            " duration_s=%.3f udp_mbps=%.3f payload_mbps=%.3f",
            CONFIG_APP_NODE_ID,
            TEST_DESTINATION_MODE_TEXT,
            transport_ip,
            tx_packets,
            tx_attempts,
            tx_errors,
            last_send_errno,
            tx_udp_payload_bytes,
            tx_test_payload_bytes,
            duration_s,
            udp_mbps,
            payload_mbps);
    ESP_LOGI(TAG, "===============================================");

    free(tx_buf);
    close(sock);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Application entry point for the LAN8651 10BASE-T1S traffic-pattern test.
 *
 * The function initializes ESP-IDF networking, configures the static Ethernet
 * interface, initializes the SPI-connected LAN8651 MAC-PHY, applies PLCA
 * settings, attaches the Ethernet driver to esp-netif, registers event
 * handlers, starts traffic-pattern tasks, and starts the Ethernet driver.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "Starting LAN8651 10BASE-T1S traffic-pattern firmware");
    ESP_LOGI(TAG, "Node ID: %d", CONFIG_APP_NODE_ID);
    ESP_LOGI(TAG, "Static IP: 192.168.50.%d", APP_IP_LAST_OCTET);
    ESP_LOGI(TAG, "Configured PLCA node count: %d", CONFIG_APP_NODE_COUNT);
    ESP_LOGI(TAG, "Sender mask: %d", CONFIG_APP_TEST_SENDER_MASK);
    ESP_LOGI(TAG, "Destination mode: %s", TEST_DESTINATION_MODE_TEXT);
#if CONFIG_APP_TEST_DEST_UNICAST
    ESP_LOGI(TAG, "Application destination node ID: %d", CONFIG_APP_TEST_DEST_NODE_ID);
#endif
    ESP_LOGI(TAG, "UDP transport IP: %s", TEST_BROADCAST_IP);
    ESP_LOGI(TAG, "Test duration: %d s", CONFIG_APP_TEST_DURATION_S);
    ESP_LOGI(TAG, "Test start delay: %d s", CONFIG_APP_TEST_START_DELAY_S);
    ESP_LOGI(TAG, "Test payload: %d B", CONFIG_APP_TEST_PAYLOAD_BYTES);
    ESP_LOGI(TAG, "Test TX delay: %d us", CONFIG_APP_TEST_TX_DELAY_US);
    ESP_LOGI(TAG, "Test UDP port: %d", CONFIG_APP_TEST_UDP_PORT);
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

    if (!is_local_node_sender()) {
        BaseType_t rx_task_created = xTaskCreate(traffic_rx_task, "traffic_rx",
                                                4096, NULL, 5, NULL);
        if (rx_task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create traffic_rx task");
            stop_on_error();
        }
    } else {
        ESP_LOGI(TAG, "RX task not started on this node because it is configured as a sender");
    }

    BaseType_t tx_task_created = xTaskCreate(traffic_tx_task, "traffic_tx",
                                            4096, NULL, 5, NULL);
    if (tx_task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create traffic_tx task");
        stop_on_error();
    }

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
}