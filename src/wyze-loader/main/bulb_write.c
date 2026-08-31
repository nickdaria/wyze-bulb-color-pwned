#include "bulb_write.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define BULB_I2C_PORT     I2C_NUM_0
#define BULB_I2C_SCL      18
#define BULB_I2C_SDA      19
#define BULB_I2C_HZ       200000
#define BULB_ADDR_CFG     0xB0
#define BULB_GRAY_BASE    0x5B
#define BULB_ENABLE_MASK  0x1F
#define BULB_CURRENT      0x1E
#define BULB_GRAY_MAX     0x03FF
#define BULB_NUM_CH       5

static const char *TAG_BULB = "bulb_write";
static bool s_ready = false;

static const char *s_names[BULB_NUM_CH] = { "Blue", "Green", "Red", "White", "White 2" };

uint8_t bulb_channel_count(void) { return BULB_NUM_CH; }

const char *bulb_channel_name(uint8_t ch) { return ch < BULB_NUM_CH ? s_names[ch] : "?"; }

static esp_err_t bulb_cfg_write(uint8_t addr, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, addr, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t e = i2c_master_cmd_begin(BULB_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return e;
}

esp_err_t bulb_write_init(void) {
    if (!s_ready) {
        i2c_config_t c = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = BULB_I2C_SDA,
            .scl_io_num = BULB_I2C_SCL,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = BULB_I2C_HZ,
        };
        esp_err_t e = i2c_param_config(BULB_I2C_PORT, &c);
        if (e != ESP_OK) return e;
        e = i2c_driver_install(BULB_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
        s_ready = true;
    }
    esp_err_t e = bulb_cfg_write(BULB_ADDR_CFG, BULB_ENABLE_MASK);
    if (e != ESP_OK) { ESP_LOGE(TAG_BULB, "enable failed: %s", esp_err_to_name(e)); return e; }
    for (uint8_t ch = 1; ch <= BULB_NUM_CH; ch++) {
        e = bulb_cfg_write(BULB_ADDR_CFG + ch, BULB_CURRENT);
        if (e != ESP_OK) { ESP_LOGE(TAG_BULB, "current ch%u failed: %s", ch, esp_err_to_name(e)); return e; }
    }
    ESP_LOGI(TAG_BULB, "init ok: en=0x%02X cur=0x%02X", BULB_ENABLE_MASK, BULB_CURRENT);
    return ESP_OK;
}

esp_err_t bulb_write(uint8_t ch, uint16_t value) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (ch >= BULB_NUM_CH) return ESP_ERR_INVALID_ARG;
    if (value > BULB_GRAY_MAX) value = BULB_GRAY_MAX;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((BULB_GRAY_BASE + ch) << 1), true);
    i2c_master_write_byte(cmd, (uint8_t)(value & 0x1F), true);
    i2c_master_write_byte(cmd, (uint8_t)((value >> 5) & 0x1F), true);
    i2c_master_stop(cmd);
    esp_err_t e = i2c_master_cmd_begin(BULB_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return e;
}
