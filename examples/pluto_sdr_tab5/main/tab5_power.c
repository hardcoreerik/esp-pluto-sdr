#include "tab5_power.h"

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "tab5_power";

enum { SDA_PIN = 31, SCL_PIN = 32, EXPANDER = 0x44, PIN_USB = 3 };
enum { REG_DIR = 0x03, REG_OUT = 0x05, REG_HIZ = 0x07, REG_IN = 0x0F };

static esp_err_t rd(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *v) { return i2c_master_transmit_receive(d, &reg, 1, v, 1, 100); }
static esp_err_t wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t v) {
  uint8_t b[2] = {reg, v};
  return i2c_master_transmit(d, b, 2, 100);
}

bool tab5_usb_power(bool enable) {
  i2c_master_bus_handle_t bus = NULL;
  i2c_master_bus_config_t bc = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = SDA_PIN,
      .scl_io_num = SCL_PIN,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  if (i2c_new_master_bus(&bc, &bus) != ESP_OK) {
    ESP_LOGE(TAG, "I2C bus did not start");
    return false;
  }
  i2c_master_dev_handle_t d = NULL;
  i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = EXPANDER, .scl_speed_hz = 400000};
  bool ok = false;
  if (i2c_master_bus_add_device(bus, &dc, &d) == ESP_OK) {
    uint8_t dir = 0, out = 0, hiz = 0, in = 0;
    if (rd(d, REG_DIR, &dir) == ESP_OK && rd(d, REG_OUT, &out) == ESP_OK && rd(d, REG_HIZ, &hiz) == ESP_OK) {
      ESP_LOGI(TAG, "expander 0x44 before: dir=%02x out=%02x hiz=%02x", dir, out, hiz);
      const uint8_t m = (uint8_t)(1u << PIN_USB);
      /* output value first, then direction and drive, so the pin never glitches the wrong way */
      out = enable ? (uint8_t)(out | m) : (uint8_t)(out & ~m);
      hiz = (uint8_t)(hiz & ~m);
      dir = (uint8_t)(dir | m);
      if (wr(d, REG_OUT, out) == ESP_OK && wr(d, REG_HIZ, hiz) == ESP_OK && wr(d, REG_DIR, dir) == ESP_OK && rd(d, REG_OUT, &out) == ESP_OK &&
          rd(d, REG_IN, &in) == ESP_OK) {
        ESP_LOGI(TAG, "expander 0x44 after:  dir=%02x out=%02x in=%02x -> USB-A 5 V %s", dir, out, in, ((out >> PIN_USB) & 1) ? "ON" : "OFF");
        ok = (((out >> PIN_USB) & 1) != 0) == enable;
      }
    } else {
      ESP_LOGE(TAG, "the expander at 0x44 did not answer");
    }
    i2c_master_bus_rm_device(d);
  }
  i2c_del_master_bus(bus);
  return ok;
}
