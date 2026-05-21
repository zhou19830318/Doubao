/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 */

/**
 * @file captive_portal.c
 * @brief Captive Portal implementation for WiFi provisioning
 * 
 * This module implements a captive portal that redirects all DNS queries
 * to the device's AP IP address (192.168.4.1), forcing mobile devices
 * to automatically open the configuration page.
 */

#include "captive_portal.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include <string.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "captive_portal";

#define DNS_PORT 53
#define DNS_MAX_SIZE 512
#define MAX_CLIENTS 4

typedef struct {
    int sock_fd;
    TaskHandle_t task_handle;
    bool running;
    bool first_request_logged;  // Track if we've logged the first request
} dns_server_t;

static dns_server_t s_dns_server = {0};
static esp_netif_t *s_ap_netif = NULL;

/**
 * @brief DNS packet header structure
 */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

/**
 * @brief Process DNS query and craft response
 */
static void process_dns_query(int sock_fd, struct sockaddr_in *client_addr, 
                               socklen_t client_len, uint8_t *buffer, size_t len)
{
    if (len < sizeof(dns_header_t)) {
        return;
    }

    dns_header_t *header = (dns_header_t *)buffer;
    
    // Only process standard queries
    if ((header->flags & htons(0x8000)) != 0) {
        return;  // Not a query
    }
    
    // Extract domain name from query for logging
    char domain_name[64] = {0};
    uint8_t *ptr = buffer + sizeof(dns_header_t);
    int offset = 0;
    while (*ptr != 0 && offset < 60) {
        int label_len = *ptr;
        if (label_len > 63) break;
        ptr++;
        if (offset > 0) {
            strncat(domain_name, ".", sizeof(domain_name) - 1);
        }
        strncat(domain_name, (char*)ptr, MIN(label_len, sizeof(domain_name) - strlen(domain_name) - 1));
        ptr += label_len;
        offset += label_len + 1;
    }
    ESP_LOGI(TAG, "DNS query for: %s", domain_name);
    
    // Log first request at INFO level for visibility
    if (!s_dns_server.first_request_logged) {
        ESP_LOGI(TAG, "First DNS request received from client!");
        s_dns_server.first_request_logged = true;
    }

    // Craft DNS response
    // Set QR bit (response) and RCODE (no error)
    header->flags = htons(0x8580);  // QR=1, AA=1, RCODE=0
    
    // Set answer count to 1
    header->qdcount = htons(1);
    header->ancount = htons(1);
    header->nscount = 0;
    header->arcount = 0;

    // Find the end of question section
    uint8_t *answer_ptr = buffer + len;
    
    // Add answer section: point to our AP IP (192.168.4.1)
    // Format: [name pointer][type][class][TTL][data length][IP address]
    uint8_t answer[] = {
        0xC0, 0x0C,              // Name pointer to question name
        0x00, 0x01,              // Type: A record
        0x00, 0x01,              // Class: IN
        0x00, 0x00, 0x00, 0x3C,  // TTL: 60 seconds
        0x00, 0x04,              // Data length: 4 bytes (IPv4)
        192, 168, 4, 1           // IP: 192.168.4.1
    };
    
    memcpy(answer_ptr, answer, sizeof(answer));
    
    size_t response_len = len + sizeof(answer);
    
    // Send response back to client
    ssize_t sent = sendto(sock_fd, buffer, response_len, 0, 
           (struct sockaddr *)client_addr, client_len);
    
    if (sent > 0) {
        ESP_LOGD(TAG, "DNS response sent: %d bytes", sent);
    } else {
        ESP_LOGW(TAG, "DNS sendto failed: errno %d", errno);
    }
}

/**
 * @brief DNS server task
 */
static void dns_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr;
    uint8_t buffer[DNS_MAX_SIZE];
    struct sockaddr_in client_addr;
    socklen_t client_len;
    
    ESP_LOGI(TAG, "DNS server starting on port %d", DNS_PORT);
    
    // Create UDP socket
    s_dns_server.sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_server.sock_fd < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        s_dns_server.running = false;
        vTaskDelete(NULL);
        return;
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(s_dns_server.sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind to port 53
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);
    
    if (bind(s_dns_server.sock_fd, (struct sockaddr *)&server_addr, 
             sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket: errno %d", errno);
        close(s_dns_server.sock_fd);
        s_dns_server.running = false;
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "DNS server listening on *:53");
    s_dns_server.running = true;
    s_dns_server.first_request_logged = false;
    
    // Main loop
    while (s_dns_server.running) {
        client_len = sizeof(client_addr);
        ssize_t len = recvfrom(s_dns_server.sock_fd, buffer, sizeof(buffer), 0,
                               (struct sockaddr *)&client_addr, &client_len);
        
        if (len > 0) {
            process_dns_query(s_dns_server.sock_fd, &client_addr, 
                            client_len, buffer, len);
        } else if (len < 0) {
            ESP_LOGW(TAG, "recvfrom error: errno %d", errno);
        }
    }
    
    close(s_dns_server.sock_fd);
    s_dns_server.sock_fd = -1;
    ESP_LOGI(TAG, "DNS server stopped");
    vTaskDelete(NULL);
}

/**
 * @brief Start captive portal DNS server
 */
esp_err_t captive_portal_start(void)
{
    if (s_dns_server.running) {
        ESP_LOGW(TAG, "Captive portal already running");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Create DNS server task
    BaseType_t ret = xTaskCreate(dns_server_task, "dns_server", 
                                  4096, NULL, 5, &s_dns_server.task_handle);
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DNS server task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Captive portal started");
    return ESP_OK;
}

/**
 * @brief Stop captive portal DNS server
 */
esp_err_t captive_portal_stop(void)
{
    if (!s_dns_server.running) {
        return ESP_OK;
    }
    
    s_dns_server.running = false;
    
    // Wait for task to finish
    if (s_dns_server.task_handle) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_dns_server.task_handle = NULL;
    }
    
    ESP_LOGI(TAG, "Captive portal stopped");
    return ESP_OK;
}

/**
 * @brief Check if captive portal is running
 */
bool captive_portal_is_running(void)
{
    return s_dns_server.running;
}
