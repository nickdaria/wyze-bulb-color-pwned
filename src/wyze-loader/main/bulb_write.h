#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t bulb_write_init(void);
esp_err_t bulb_write(uint8_t ch, uint16_t value);
uint8_t bulb_channel_count(void);
const char *bulb_channel_name(uint8_t ch);
