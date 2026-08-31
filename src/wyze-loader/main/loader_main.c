#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "bulb_write.h"


#define AP_SSID_PREFIX "wyze-loader"
#define CHUNKSZ 4096

static const char *TAG = "loader";
static char last_target[32] = {0};
static uint8_t s_mac[6] = {0};

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get(httpd_req_t *req){
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                          index_html_end - index_html_start);
}

static esp_err_t info_get(httpd_req_t *req){
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *app = esp_app_get_description();
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"running\":\"%s\",\"target\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"version\":\"%s\",\"date\":\"%s\",\"bonus\":\"Roll Tide\"}",
             run ? run->label : "?", upd ? upd->label : "?",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5],
             app ? app->version : "?", app ? app->date : "?");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t parts_get(httpd_req_t *req){
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    bool first = true;
    char row[220];
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        snprintf(row, sizeof(row),
                 "%s{\"label\":\"%s\",\"type\":%d,\"subtype\":%d,\"offset\":%lu,\"size\":%lu}",
                 first ? "" : ",", p->label, (int)p->type, (int)p->subtype,
                 (unsigned long)p->address, (unsigned long)p->size);
        httpd_resp_sendstr_chunk(req, row);
        first = false;
        it = esp_partition_next(it);
    }
    if (it) esp_partition_iterator_release(it);
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t dump_get(httpd_req_t *req){
    char q[128] = {0};
    char name[40] = {0}, offs[20] = {0}, szs[20] = {0};
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    bool have_name = false, have_off = false;
    if (qlen > 1 && qlen <= sizeof(q)) {
        httpd_req_get_url_query_str(req, q, qlen);
        if (httpd_query_key_value(q, "name", name, sizeof(name)) == ESP_OK) have_name = true;
        if (httpd_query_key_value(q, "offset", offs, sizeof(offs)) == ESP_OK &&
            httpd_query_key_value(q, "size", szs, sizeof(szs)) == ESP_OK) have_off = true;
    }
    uint8_t *buf = malloc(CHUNKSZ);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/octet-stream");
    char cd[80];

    if (have_name) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, name);
        if (!p) { free(buf); httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such partition"); return ESP_FAIL; }
        snprintf(cd, sizeof(cd), "attachment; filename=%s.bin", p->label);
        httpd_resp_set_hdr(req, "Content-Disposition", cd);
        size_t off = 0;
        while (off < p->size) {
            size_t n = (p->size - off) < CHUNKSZ ? (p->size - off) : CHUNKSZ;
            if (esp_partition_read(p, off, buf, n) != ESP_OK) break;
            if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) break;
            off += n;
        }
    } else if (have_off) {
        uint32_t addr = strtoul(offs, NULL, 0);
        uint32_t sz = strtoul(szs, NULL, 0);
        snprintf(cd, sizeof(cd), "attachment; filename=flash_0x%lx_0x%lx_%02X%02X%02X.bin", (unsigned long)addr, (unsigned long)sz, s_mac[3], s_mac[4], s_mac[5]);
        httpd_resp_set_hdr(req, "Content-Disposition", cd);
        uint32_t off = 0;
        while (off < sz) {
            uint32_t n = (sz - off) < CHUNKSZ ? (sz - off) : CHUNKSZ;
            if (esp_flash_read(NULL, buf, addr + off, n) != ESP_OK) break;
            if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) break;
            off += n;
        }
    } else {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use ?name= or ?offset=&size=");
        return ESP_FAIL;
    }
    free(buf);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t ota_post(httpd_req_t *req){
    const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL);
    if (!upd) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota slot"); return ESP_FAIL; }
    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(upd, OTA_SIZE_UNKNOWN, &h);
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err)); return ESP_FAIL; }
    uint8_t *buf = malloc(CHUNKSZ);
    if (!buf) { esp_ota_abort(h); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_FAIL; }
    int remaining = req->content_len;
    int total = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, (char *)buf, remaining < CHUNKSZ ? remaining : CHUNKSZ);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(buf); esp_ota_abort(h); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed"); return ESP_FAIL; }
        err = esp_ota_write(h, buf, r);
        if (err != ESP_OK) { free(buf); esp_ota_abort(h); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err)); return ESP_FAIL; }
        remaining -= r;
        total += r;
    }
    free(buf);
    err = esp_ota_end(h);
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err)); return ESP_FAIL; }
    strncpy(last_target, upd->label, sizeof(last_target) - 1);
    char msg[128];
    snprintf(msg, sizeof(msg), "OK: wrote %d bytes to %s. Click 'Set boot to target' then 'Reboot'.", total, upd->label);
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t setboot_post(httpd_req_t *req){
    char q[64] = {0}, name[40] = {0};
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1 && qlen <= sizeof(q)) {
        httpd_req_get_url_query_str(req, q, qlen);
        httpd_query_key_value(q, "name", name, sizeof(name));
    }
    const char *target = name[0] ? name : (last_target[0] ? last_target : NULL);
    const esp_partition_t *p = NULL;
    if (target) p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, target);
    else p = esp_ota_get_next_update_partition(NULL);
    if (!p) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no target partition"); return ESP_FAIL; }
    esp_err_t err = esp_ota_set_boot_partition(p);
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: boot set to %s", err == ESP_OK ? "OK" : esp_err_to_name(err), p->label);
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t reboot_post(httpd_req_t *req){
    httpd_resp_sendstr(req, "rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t bulb_init_post(httpd_req_t *req){
    esp_err_t e = bulb_write_init();
    char msg[64];
    snprintf(msg, sizeof(msg), "%s: bulb i2c init", e == ESP_OK ? "OK" : esp_err_to_name(e));
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t bulb_write_post(httpd_req_t *req){
    char q[64] = {0}, chs[8] = {0}, vals[8] = {0};
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1 && qlen <= sizeof(q)) {
        httpd_req_get_url_query_str(req, q, qlen);
        httpd_query_key_value(q, "ch", chs, sizeof(chs));
        httpd_query_key_value(q, "val", vals, sizeof(vals));
    }
    if (!chs[0] || !vals[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use ?ch=&val="); return ESP_FAIL; }
    int ch = atoi(chs);
    long val = strtol(vals, NULL, 0);
    if (ch < 0 || ch > 4) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ch 0-4"); return ESP_FAIL; }
    esp_err_t e = bulb_write((uint8_t)ch, (uint16_t)val);
    char msg[80];
    snprintf(msg, sizeof(msg), "%s: OUT%d=%ld", e == ESP_OK ? "OK" : esp_err_to_name(e), ch + 1, val);
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t bulb_channels_get(httpd_req_t *req){
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    char row[64];
    uint8_t n = bulb_channel_count();
    for (uint8_t i = 0; i < n; i++) {
        snprintf(row, sizeof(row), "%s{\"idx\":%u,\"name\":\"%s\"}", i ? "," : "", i, bulb_channel_name(i));
        httpd_resp_sendstr_chunk(req, row);
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void start_httpd(void){
    httpd_handle_t s = NULL;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 8192;
    c.max_uri_handlers = 12;
    c.recv_wait_timeout = 20;
    c.send_wait_timeout = 20;
    c.lru_purge_enable = true;
    if (httpd_start(&s, &c) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }
    httpd_uri_t u;
    u = (httpd_uri_t){ .uri = "/", .method = HTTP_GET, .handler = root_get };       httpd_register_uri_handler(s, &u);
    u = (httpd_uri_t){ .uri = "/info", .method = HTTP_GET, .handler = info_get };    httpd_register_uri_handler(s, &u);
    u = (httpd_uri_t){ .uri = "/parts", .method = HTTP_GET, .handler = parts_get };  httpd_register_uri_handler(s, &u);
    u = (httpd_uri_t){ .uri = "/dump", .method = HTTP_GET, .handler = dump_get };    httpd_register_uri_handler(s, &u);
    u = (httpd_uri_t){ .uri = "/ota", .method = HTTP_POST, .handler = ota_post };    httpd_register_uri_handler(s, &u);
    u = (httpd_uri_t){ .uri = "/setboot", .method = HTTP_POST, .handler = setboot_post }; httpd_register_uri_handler(s, &u);
    u = (httpd_uri_t){ .uri = "/reboot", .method = HTTP_POST, .handler = reboot_post };   httpd_register_uri_handler(s, &u);
    u = (httpd_uri_t){ .uri = "/bulb/init", .method = HTTP_POST, .handler = bulb_init_post }; httpd_register_uri_handler(s, &u);
    u = (httpd_uri_t){ .uri = "/bulb/write", .method = HTTP_POST, .handler = bulb_write_post }; httpd_register_uri_handler(s, &u);
    u = (httpd_uri_t){ .uri = "/bulb/channels", .method = HTTP_GET, .handler = bulb_channels_get }; httpd_register_uri_handler(s, &u);
}

static void start_ap(void){
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_read_mac(s_mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), AP_SSID_PREFIX "_%02X%02X%02X", s_mac[3], s_mac[4], s_mac[5]);
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_max_tx_power(34);
    ESP_LOGI(TAG, "AP up: %s  http://192.168.4.1/", ap_ssid);
}

void app_main(void){
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    start_ap();
    start_httpd();
    bulb_write_init();
    bulb_write(0, 29);
    bulb_write(1, 0);
    bulb_write(2, 23);
    bulb_write(3, 0);
    bulb_write(4, 0);
}
