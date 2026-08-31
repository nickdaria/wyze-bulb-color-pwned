#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define AP_SSID   "ASDFGHJKLzxcvb"
#define AP_PASS   "0x82562647"
#define AP_CHANNEL 1
#define SERVER_PORT 8070
#define FACTORY_UDP_PORT 22223
#define OTA_URL   "http://192.168.3.168:8070/wyze_iot_service.bin"
#define TRIGGER_DELAY_MS 4000
#define TRIGGER_ATTEMPTS 4

static const char *TAG = "program-ap";

extern const uint8_t fw_start[] asm("_binary_payload_bin_start");
extern const uint8_t fw_end[]   asm("_binary_payload_bin_end");

static volatile uint32_t g_bulb_ip = 0;

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGW(TAG, ">>> STATION JOINED %02x:%02x:%02x:%02x:%02x:%02x aid=%d",
            e->mac[0],e->mac[1],e->mac[2],e->mac[3],e->mac[4],e->mac[5], e->aid);
    }
}

static void ip_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    ip_event_assigned_ip_to_client_t *e = data;
    char ipstr[16]; esp_ip4addr_ntoa(&e->ip, ipstr, sizeof(ipstr));
    ESP_LOGW(TAG, ">>> DHCP LEASE -> %s", ipstr);
    g_bulb_ip = e->ip.addr;
}

static void udp_trigger_task(void *arg) {
    char msg[192];
    int n = snprintf(msg, sizeof(msg), "hap upgrade %s", OTA_URL);
    for (;;) {
        while (g_bulb_ip == 0) vTaskDelay(pdMS_TO_TICKS(200));
        uint32_t ip = g_bulb_ip;
        g_bulb_ip = 0;
        vTaskDelay(pdMS_TO_TICKS(TRIGGER_DELAY_MS));

        int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int brd = 1; setsockopt(s, SOL_SOCKET, SO_BROADCAST, &brd, sizeof(brd));
        struct timeval tv = {1, 0}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in dst = {0}, bcast = {0};
        dst.sin_family = AF_INET; dst.sin_port = htons(FACTORY_UDP_PORT); dst.sin_addr.s_addr = ip;
        bcast.sin_family = AF_INET; bcast.sin_port = htons(FACTORY_UDP_PORT);
        bcast.sin_addr.s_addr = htonl((192<<24)|(168<<16)|(3<<8)|255);

        ESP_LOGW(TAG, "=== firing UDP trigger to bulb ===");
        ESP_LOGW(TAG, "    payload: \"%s\"", msg);

        for (int i = 1; i <= TRIGGER_ATTEMPTS; i++) {
            sendto(s, msg, n, 0, (struct sockaddr *)&dst, sizeof(dst));
            ESP_LOGW(TAG, "    sent trigger attempt %d/%d", i, TRIGGER_ATTEMPTS);
            char rx[128]; struct sockaddr_in from; socklen_t fl = sizeof(from);
            int r = recvfrom(s, rx, sizeof(rx)-1, 0, (struct sockaddr *)&from, &fl);
            if (r > 0) {
                rx[r] = 0;
                char fs[16]; inet_ntoa_r(from.sin_addr, fs, sizeof(fs));
                ESP_LOGW(TAG, ">>> BULB REPLY from %s: \"%s\"", fs, rx);
                if (strncmp(rx, "upgrading", 9) == 0) {
                    ESP_LOGW(TAG, "    bulb accepted — stopping burst");
                    break;
                }
            } else {
                ESP_LOGI(TAG, "    (no reply this attempt)");
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        close(s);
        ESP_LOGW(TAG, "=== trigger burst done; watch for HTTP GET ===");
    }
}

static void serve_client_task(void *arg) {
    int fd = (int)(intptr_t)arg;
    size_t fw_len = fw_end - fw_start;
    char req[512]; recv(fd, req, sizeof(req)-1, 0);
    ESP_LOGW(TAG, ">>> HTTP request; serving %u bytes (Content-Length, HTTP/1.0)", (unsigned)fw_len);
    char hdr[160];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %u\r\n\r\n",
        (unsigned)fw_len);
    send(fd, hdr, hlen, 0);
    const uint8_t *p = fw_start; size_t rem = fw_len; bool ok = true;
    while (rem > 0) {
        size_t n = rem < 4096 ? rem : 4096;
        ssize_t s = send(fd, p, n, 0);
        if (s < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            ESP_LOGE(TAG, "send err %d at offset %u", errno, (unsigned)(p - fw_start));
            ok = false; break;
        }
        p += s; rem -= s;
    }
    close(fd);
    ESP_LOGW(TAG, "transfer %s (%u bytes sent)", ok ? "COMPLETE" : "FAILED", (unsigned)(p - fw_start));
    vTaskDelete(NULL);
}

static void raw_http_server_task(void *arg) {
    size_t fw_len = fw_end - fw_start;
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(SERVER_PORT); addr.sin_addr.s_addr = INADDR_ANY;
    bind(ls, (struct sockaddr *)&addr, sizeof(addr));
    listen(ls, 4);
    ESP_LOGW(TAG, "HTTP/1.0 server on 192.168.3.168:%d  fw=%u bytes", SERVER_PORT, (unsigned)fw_len);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        xTaskCreate(serve_client_task, "http_client", 4096, (void *)(intptr_t)fd, 5, NULL);
    }
}

static void heartbeat_task(void *arg) {
    uint32_t sec = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        sec += 5;
        ESP_LOGW(TAG, "-- heartbeat: uptime %us, free heap %u, bulb_ip %s --",
            sec, (unsigned)esp_get_free_heap_size(), g_bulb_ip ? "pending" : "none");
    }
}

static void start_ap(void){
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap));
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip, 192, 168, 3, 168);
    IP4_ADDR(&ip.gw, 192, 168, 3, 168);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, ip_evt, NULL));

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(AP_SSID);
    strncpy((char *)ap_cfg.ap.password, AP_PASS, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP up: SSID=\"%s\" PASS=\"%s\" ch=%d ip=192.168.3.168/24", AP_SSID, AP_PASS, AP_CHANNEL);
}

void app_main(void){
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGW(TAG, "=== wyze program-ap starting ===");
    start_ap();
    xTaskCreate(raw_http_server_task, "http_srv", 4096, NULL, 5, NULL);
    xTaskCreate(udp_trigger_task, "udp_trig", 4096, NULL, 5, NULL);
    ESP_LOGW(TAG, "READY. Waiting for bulb to join SSID \"%s\" ...", AP_SSID);
    //xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 3, NULL);
}