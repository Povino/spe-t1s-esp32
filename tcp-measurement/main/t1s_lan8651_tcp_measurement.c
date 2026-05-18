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
#define TEST_MAGIC         0x54315354U
#define TEST_VERSION       1
#define TEST_MAX_NODES     8
#define TCP_MAX_CONNECT_RETRIES 20
#define TCP_RECV_BUFFER_BYTES   512

static volatile bool s_got_ip = false;

/*
 * Application record used inside the TCP byte stream.
 * TCP does not preserve message boundaries, so the receiver reconstructs
 * fixed-size records from the incoming stream before processing headers.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;
    uint16_t payload_len;
    uint16_t reserved;
    uint16_t record_len;
    uint32_t src_node_id;
    uint32_t seq;
    uint32_t tx_time_ms;
} tcp_record_header_t;

typedef struct {
    int sock;
    struct sockaddr_in peer_addr;
} tcp_connection_arg_t;

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
 * options still need runtime validation. The node ID, TCP server ID, and
 * TCP client mask must match the configured topology. The TCP record length
 * must also fit into the 16-bit record length field used in the test header.
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

    if (CONFIG_APP_TCP_SERVER_ID >= CONFIG_APP_NODE_COUNT) {
        ESP_LOGE(TAG, "Invalid configuration: APP_TCP_SERVER_ID=%d, APP_NODE_COUNT=%d",
                 CONFIG_APP_TCP_SERVER_ID, CONFIG_APP_NODE_COUNT);
        stop_on_error();
    }

    uint32_t valid_node_mask = (1U << CONFIG_APP_NODE_COUNT) - 1U;
    if ((CONFIG_APP_TCP_CLIENT_MASK & ~valid_node_mask) != 0) {
        ESP_LOGE(TAG, "Invalid configuration: APP_TCP_CLIENT_MASK=%d references nodes outside APP_NODE_COUNT=%d",
                 CONFIG_APP_TCP_CLIENT_MASK, CONFIG_APP_NODE_COUNT);
        stop_on_error();
    }

    const size_t record_len = sizeof(tcp_record_header_t) + CONFIG_APP_TCP_PAYLOAD_BYTES;
    if (record_len > UINT16_MAX) {
        ESP_LOGE(TAG, "Invalid configuration: TCP record length=%u exceeds uint16_t storage",
                 (unsigned int)record_len);
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
 * interface as ready for the TCP measurement tasks.
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
 * @brief Check whether the current node is selected as a TCP client.
 *
 * @return true when the current node ID is enabled in APP_TCP_CLIENT_MASK,
 *         false otherwise.
 */
static bool is_local_node_client(void)
{
    return (CONFIG_APP_TCP_CLIENT_MASK & (1U << CONFIG_APP_NODE_ID)) != 0;
}

/**
 * @brief Build the IPv4 address for a node ID in the test subnet.
 *
 * @param node_id Node ID used to derive the last IPv4 octet.
 * @param buffer Output buffer for the textual IPv4 address.
 * @param buffer_len Length of the output buffer.
 * @return true when the address was written successfully, false otherwise.
 */
static bool build_node_ip(int node_id, char *buffer, size_t buffer_len)
{
    int written = snprintf(buffer, buffer_len, "192.168.50.%d", node_id + 1);
    return written > 0 && written < (int)buffer_len;
}

/**
 * @brief Send all bytes from a buffer over a TCP socket.
 *
 * The function repeats send() until the complete buffer is transmitted or
 * an error occurs.
 *
 * @param sock TCP socket.
 * @param buffer Data buffer to transmit.
 * @param length Number of bytes to transmit.
 * @return ESP_OK when all bytes are sent, otherwise ESP_FAIL.
 */
static esp_err_t send_all(int sock, const uint8_t *buffer, size_t length)
{
    size_t sent_total = 0;

    while (sent_total < length) {
        int sent = send(sock, buffer + sent_total, length - sent_total, 0);
        if (sent <= 0) {
            return ESP_FAIL;
        }

        sent_total += (size_t)sent;
    }

    return ESP_OK;
}

/**
 * @brief Process one complete TCP application record on the server side.
 *
 * @param record Buffer containing one complete TCP application record.
 * @param record_len Length of the complete application record.
 * @param src_node_id Output source node ID read from the record header.
 * @param seq Output sequence number read from the record header.
 * @param payload_len Output payload length read from the record header.
 * @return true when the record header is valid, false otherwise.
 */
static bool parse_tcp_record(const uint8_t *record, size_t record_len,
                             uint32_t *src_node_id, uint32_t *seq,
                             uint16_t *payload_len)
{
    if (record_len < sizeof(tcp_record_header_t)) {
        return false;
    }

    tcp_record_header_t hdr;
    memcpy(&hdr, record, sizeof(hdr));

    uint32_t magic = ntohl(hdr.magic);
    uint16_t version = ntohs(hdr.version);
    uint16_t header_len = ntohs(hdr.header_len);
    uint16_t payload = ntohs(hdr.payload_len);
    uint16_t declared_record_len = ntohs(hdr.record_len);
    uint32_t source = ntohl(hdr.src_node_id);
    uint32_t sequence = ntohl(hdr.seq);

    if (magic != TEST_MAGIC || version != TEST_VERSION) {
        return false;
    }

    if (header_len != sizeof(tcp_record_header_t)) {
        return false;
    }

    if ((size_t)declared_record_len != record_len) {
        return false;
    }

    if (payload != CONFIG_APP_TCP_PAYLOAD_BYTES) {
        return false;
    }

    if (source >= TEST_MAX_NODES) {
        return false;
    }

    *src_node_id = source;
    *seq = sequence;
    *payload_len = payload;

    return true;
}

/**
 * @brief Handle one accepted TCP client connection.
 *
 * The task reconstructs fixed-size application records from the TCP stream,
 * validates their headers, tracks sequence numbers and byte counters, and
 * prints receiver-side TCP results when the client closes the connection.
 *
 * @param arg Pointer to tcp_connection_arg_t allocated by the server task.
 */
static void tcp_connection_task(void *arg)
{
    tcp_connection_arg_t *conn_arg = (tcp_connection_arg_t *)arg;
    int sock = conn_arg->sock;
    struct sockaddr_in peer_addr = conn_arg->peer_addr;
    free(conn_arg);

    const size_t record_len = sizeof(tcp_record_header_t) + CONFIG_APP_TCP_PAYLOAD_BYTES;

    uint8_t *record = malloc(record_len);
    uint8_t *rx_buf = malloc(TCP_RECV_BUFFER_BYTES);

    if (record == NULL || rx_buf == NULL) {
        ESP_LOGE(TAG, "TCP RX buffer allocation failed");
        free(record);
        free(rx_buf);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    char peer_ip[16] = {0};
    inet_ntoa_r(peer_addr.sin_addr, peer_ip, sizeof(peer_ip));
    ESP_LOGI(TAG, "Accepted TCP client from %s:%u", peer_ip, ntohs(peer_addr.sin_port));

    bool source_seen = false;
    uint32_t source_node = 0;
    uint32_t first_seq = 0;
    uint32_t last_seq = 0;
    uint64_t records = 0;
    uint64_t invalid_records = 0;
    uint64_t tcp_record_bytes = 0;
    uint64_t test_payload_bytes = 0;
    int64_t first_rx_us = 0;
    int64_t last_rx_us = 0;
    size_t record_offset = 0;

    while (1) {
        int received = recv(sock, rx_buf, TCP_RECV_BUFFER_BYTES, 0);
        if (received < 0) {
            ESP_LOGW(TAG, "TCP recv failed: errno=%d", errno);
            break;
        }

        if (received == 0) {
            break;
        }

        size_t input_offset = 0;
        while (input_offset < (size_t)received) {
            size_t remaining_input = (size_t)received - input_offset;
            size_t remaining_record = record_len - record_offset;
            size_t copy_len = remaining_input < remaining_record ? remaining_input : remaining_record;

            memcpy(record + record_offset, rx_buf + input_offset, copy_len);
            record_offset += copy_len;
            input_offset += copy_len;

            if (record_offset == record_len) {
                uint32_t src_node_id = 0;
                uint32_t seq = 0;
                uint16_t payload_len = 0;

                if (parse_tcp_record(record, record_len, &src_node_id, &seq, &payload_len)) {
                    int64_t now_us = esp_timer_get_time();

                    if (!source_seen) {
                        source_seen = true;
                        source_node = src_node_id;
                        first_seq = seq;
                        first_rx_us = now_us;
                    } else if (src_node_id != source_node) {
                        invalid_records++;
                        record_offset = 0;
                        continue;
                    }

                    last_seq = seq;
                    last_rx_us = now_us;
                    records++;
                    tcp_record_bytes += record_len;
                    test_payload_bytes += payload_len;
                } else {
                    invalid_records++;
                }

                record_offset = 0;
            }
        }
    }

    if (record_offset != 0) {
        invalid_records++;
    }

    uint64_t expected_records = 0;
    uint64_t missing_records = 0;

    if (source_seen && last_seq >= first_seq) {
        expected_records = (uint64_t)(last_seq - first_seq + 1);
        if (expected_records >= records) {
            missing_records = expected_records - records;
        }
    }

    double duration_s = 0.0;
    if (last_rx_us > first_rx_us) {
        duration_s = (double)(last_rx_us - first_rx_us) / 1000000.0;
    }

    double record_mbps = 0.0;
    double payload_mbps = 0.0;

    if (duration_s > 0.0) {
        record_mbps = ((double)tcp_record_bytes * 8.0) / duration_s / 1000000.0;
        payload_mbps = ((double)test_payload_bytes * 8.0) / duration_s / 1000000.0;
    }

    ESP_LOGI(TAG, "========== TCP RX RESULT local_node=%d ==========", CONFIG_APP_NODE_ID);
    ESP_LOGI(TAG,
             "RESULT_TCP_RX local_node=%d src_node=%" PRIu32 " records=%" PRIu64
             " expected_records=%" PRIu64 " missing_records=%" PRIu64 " invalid_records=%" PRIu64
             " record_bytes=%" PRIu64 " payload_bytes=%" PRIu64
             " duration_s=%.3f record_mbps=%.3f payload_mbps=%.3f",
             CONFIG_APP_NODE_ID,
             source_node,
             records,
             expected_records,
             missing_records,
             invalid_records,
             tcp_record_bytes,
             test_payload_bytes,
             duration_s,
             record_mbps,
             payload_mbps);
    ESP_LOGI(TAG, "===============================================");

    free(record);
    free(rx_buf);
    close(sock);
    vTaskDelete(NULL);
}

/**
 * @brief Start the TCP server on the configured server node.
 *
 * The server listens on APP_TCP_PORT and creates one FreeRTOS task for each
 * accepted client connection. Non-server nodes terminate this task.
 *
 * @param arg FreeRTOS task argument. Not used.
 */
static void tcp_server_task(void *arg)
{
    (void)arg;

    while (!s_got_ip) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (CONFIG_APP_NODE_ID != CONFIG_APP_TCP_SERVER_ID) {
        vTaskDelete(NULL);
        return;
    }

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "TCP server socket creation failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        ESP_LOGW(TAG, "SO_REUSEADDR failed: errno=%d", errno);
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(CONFIG_APP_TCP_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "TCP server bind failed: errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, CONFIG_APP_NODE_COUNT) < 0) {
        ESP_LOGE(TAG, "TCP server listen failed: errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP server listening on port %d", CONFIG_APP_TCP_PORT);

    while (1) {
        tcp_connection_arg_t *conn_arg = calloc(1, sizeof(tcp_connection_arg_t));
        if (conn_arg == NULL) {
            ESP_LOGE(TAG, "TCP connection argument allocation failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        socklen_t addr_len = sizeof(conn_arg->peer_addr);
        conn_arg->sock = accept(listen_sock, (struct sockaddr *)&conn_arg->peer_addr, &addr_len);

        if (conn_arg->sock < 0) {
            ESP_LOGW(TAG, "TCP accept failed: errno=%d", errno);
            free(conn_arg);
            continue;
        }

        BaseType_t task_created = xTaskCreate(tcp_connection_task, "tcp_conn", 4096,
                                              conn_arg, 5, NULL);
        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create TCP connection task");
            close(conn_arg->sock);
            free(conn_arg);
        }
    }
}

/**
 * @brief Run the TCP client on nodes selected by APP_TCP_CLIENT_MASK.
 *
 * The client connects to the configured server node, sends fixed-size
 * application records for the configured duration, and prints final transmit
 * counters and throughput values. Nodes not selected as clients terminate this
 * task.
 *
 * @param arg FreeRTOS task argument. Not used.
 */
static void tcp_client_task(void *arg)
{
    (void)arg;

    while (!s_got_ip) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!is_local_node_client()) {
        ESP_LOGI(TAG, "This node is not selected as a TCP client. Client mask is %d",
                 CONFIG_APP_TCP_CLIENT_MASK);
        vTaskDelete(NULL);
        return;
    }

    if (CONFIG_APP_NODE_ID == CONFIG_APP_TCP_SERVER_ID) {
        ESP_LOGW(TAG, "This node is both server and client; client task disabled");
        vTaskDelete(NULL);
        return;
    }

    char server_ip[16] = {0};
    if (!build_node_ip(CONFIG_APP_TCP_SERVER_ID, server_ip, sizeof(server_ip))) {
        ESP_LOGE(TAG, "Failed to build TCP server IP address");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(CONFIG_APP_TCP_PORT);
    dest_addr.sin_addr.s_addr = inet_addr(server_ip);

    ESP_LOGI(TAG, "This node is TCP client");
    ESP_LOGI(TAG, "TCP server: %s:%d", server_ip, CONFIG_APP_TCP_PORT);
    ESP_LOGI(TAG, "Test starts in %d seconds", CONFIG_APP_TCP_START_DELAY_S);

    vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_TCP_START_DELAY_S * 1000));

    int sock = -1;
    bool connected = false;

    for (int attempt = 0; attempt < TCP_MAX_CONNECT_RETRIES; attempt++) {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGW(TAG, "TCP client socket creation failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) == 0) {
            connected = true;
            break;
        }

        ESP_LOGW(TAG, "TCP connect attempt %d failed: errno=%d", attempt + 1, errno);
        close(sock);
        sock = -1;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!connected) {
        ESP_LOGE(TAG, "TCP client failed to connect to server");
        vTaskDelete(NULL);
        return;
    }

    const size_t record_len = sizeof(tcp_record_header_t) + CONFIG_APP_TCP_PAYLOAD_BYTES;
    uint8_t *tx_buf = calloc(1, record_len);
    if (tx_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate TCP TX buffer");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    tcp_record_header_t *hdr = (tcp_record_header_t *)tx_buf;
    uint8_t *payload = tx_buf + sizeof(tcp_record_header_t);

    for (int i = 0; i < CONFIG_APP_TCP_PAYLOAD_BYTES; i++) {
        payload[i] = (uint8_t)(i & 0xff);
    }

    ESP_LOGI(TAG,
             "Starting TCP measurement test: client=%d server=%d duration=%d s payload=%d B tx_delay=%d us",
             CONFIG_APP_NODE_ID,
             CONFIG_APP_TCP_SERVER_ID,
             CONFIG_APP_TCP_DURATION_S,
             CONFIG_APP_TCP_PAYLOAD_BYTES,
             CONFIG_APP_TCP_TX_DELAY_US);

    uint32_t seq = 0;
    uint32_t tx_errors = 0;
    uint64_t tx_records = 0;
    uint64_t tx_record_bytes = 0;
    uint64_t tx_payload_bytes = 0;

    int64_t start_us = esp_timer_get_time();
    int64_t end_us = start_us + ((int64_t)CONFIG_APP_TCP_DURATION_S * 1000000LL);

    while (esp_timer_get_time() < end_us) {
        hdr->magic = htonl(TEST_MAGIC);
        hdr->version = htons(TEST_VERSION);
        hdr->header_len = htons(sizeof(tcp_record_header_t));
        hdr->payload_len = htons(CONFIG_APP_TCP_PAYLOAD_BYTES);
        hdr->reserved = 0;
        hdr->record_len = htons(record_len);
        hdr->src_node_id = htonl(CONFIG_APP_NODE_ID);
        hdr->seq = htonl(seq);
        hdr->tx_time_ms = htonl((uint32_t)(esp_timer_get_time() / 1000));

        if (send_all(sock, tx_buf, record_len) == ESP_OK) {
            tx_records++;
            tx_record_bytes += record_len;
            tx_payload_bytes += CONFIG_APP_TCP_PAYLOAD_BYTES;
            seq++;
        } else {
            tx_errors++;
            break;
        }

        if (CONFIG_APP_TCP_TX_DELAY_US > 0) {
            esp_rom_delay_us(CONFIG_APP_TCP_TX_DELAY_US);
        }

        if ((seq & 0x3f) == 0) {
            vTaskDelay(1);
        }
    }

    int64_t actual_end_us = esp_timer_get_time();
    double duration_s = (double)(actual_end_us - start_us) / 1000000.0;
    double record_mbps = 0.0;
    double payload_mbps = 0.0;

    if (duration_s > 0.0) {
        record_mbps = ((double)tx_record_bytes * 8.0) / duration_s / 1000000.0;
        payload_mbps = ((double)tx_payload_bytes * 8.0) / duration_s / 1000000.0;
    }

    ESP_LOGI(TAG, "========== TCP TX RESULT node=%d ==========", CONFIG_APP_NODE_ID);
    ESP_LOGI(TAG,
             "RESULT_TCP_TX node=%d server=%d records=%" PRIu64 " errors=%" PRIu32
             " record_bytes=%" PRIu64 " payload_bytes=%" PRIu64
             " duration_s=%.3f record_mbps=%.3f payload_mbps=%.3f",
             CONFIG_APP_NODE_ID,
             CONFIG_APP_TCP_SERVER_ID,
             tx_records,
             tx_errors,
             tx_record_bytes,
             tx_payload_bytes,
             duration_s,
             record_mbps,
             payload_mbps);
    ESP_LOGI(TAG, "==========================================");

    if (shutdown(sock, SHUT_RDWR) < 0) {
        ESP_LOGW(TAG, "TCP shutdown failed: errno=%d", errno);
    }

    close(sock);
    free(tx_buf);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Application entry point for the LAN8651 10BASE-T1S TCP measurement test.
 *
 * The function initializes ESP-IDF networking, configures the static Ethernet
 * interface, initializes the SPI-connected LAN8651 MAC-PHY, applies PLCA
 * settings, attaches the Ethernet driver to esp-netif, registers event
 * handlers, starts TCP server/client tasks, and starts the Ethernet driver.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "Starting LAN8651 10BASE-T1S TCP measurement firmware");
    ESP_LOGI(TAG, "Node ID: %d", CONFIG_APP_NODE_ID);
    ESP_LOGI(TAG, "Static IP: 192.168.50.%d", APP_IP_LAST_OCTET);
    ESP_LOGI(TAG, "Configured PLCA node count: %d", CONFIG_APP_NODE_COUNT);
    ESP_LOGI(TAG, "TCP server ID: %d", CONFIG_APP_TCP_SERVER_ID);
    ESP_LOGI(TAG, "TCP client mask: %d", CONFIG_APP_TCP_CLIENT_MASK);
    ESP_LOGI(TAG, "TCP duration: %d s", CONFIG_APP_TCP_DURATION_S);
    ESP_LOGI(TAG, "TCP start delay: %d s", CONFIG_APP_TCP_START_DELAY_S);
    ESP_LOGI(TAG, "TCP payload: %d B", CONFIG_APP_TCP_PAYLOAD_BYTES);
    ESP_LOGI(TAG, "TCP TX delay: %d us", CONFIG_APP_TCP_TX_DELAY_US);
    ESP_LOGI(TAG, "TCP port: %d", CONFIG_APP_TCP_PORT);
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

    BaseType_t server_task_created = xTaskCreate(tcp_server_task, "tcp_server",
                                                 4096, NULL, 5, NULL);
    if (server_task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create tcp_server task");
        stop_on_error();
    }

    BaseType_t client_task_created = xTaskCreate(tcp_client_task, "tcp_client",
                                                 4096, NULL, 5, NULL);
    if (client_task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create tcp_client task");
        stop_on_error();
    }

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
}