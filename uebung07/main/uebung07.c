#include <stdio.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MPU6050";

// ── Hardware-Konfiguration ─────────────────────────────────────────────────
#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_SDA_IO   8
#define I2C_MASTER_SCL_IO   9
#define I2C_MASTER_FREQ_HZ  100000

// ── MPU6050 ────────────────────────────────────────────────────────────────
#define MPU_ADDR            0x68    // AD0 = GND
#define MPU_REG_PWR_MGMT_1  0x6B
#define MPU_REG_ACCEL_XOUT_H 0x3B   // Start des 14-Byte Datenblocks

#define I2C_TIMEOUT_MS      1000


// ── I2C Bus initialisieren ─────────────────────────────────────────────────
static void i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_MASTER_SDA_IO,
        .scl_io_num       = I2C_MASTER_SCL_IO,
        .sda_pullup_en    = GPIO_PULLUP_DISABLE,  // Externe 4.7kΩ auf dem Breadboard
        .scl_pullup_en    = GPIO_PULLUP_DISABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0));
    ESP_LOGI(TAG, "I2C initialisiert (SDA=GPIO%d, SCL=GPIO%d, %d Hz)",
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, I2C_MASTER_FREQ_HZ);
}


// ── Ein einzelnes Register beschreiben ─────────────────────────────────────
// START → [MPU|W] → ACK → [reg] → ACK → [val] → ACK → STOP
static esp_err_t mpu_write_byte(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}


// ── Mehrere Bytes ab einem Register lesen ──────────────────────────────────
// START → [MPU|W] → ACK → [reg] → ACK
// START → [MPU|R] → ACK → ← len Bytes (alle ACK, letztes NACK) → STOP
static esp_err_t mpu_read_block(uint8_t reg, uint8_t *buf, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    // Dummy Write: interne Registeradresse setzen
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);

    // Repeated Start + Lesen
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}


// ── Hauptprogramm ──────────────────────────────────────────────────────────
void app_main(void)
{
    i2c_master_init();
    vTaskDelay(pdMS_TO_TICKS(100));

    // MPU6050 aus dem Sleep wecken (PWR_MGMT_1 = 0)
    esp_err_t ret = mpu_write_byte(MPU_REG_PWR_MGMT_1, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 nicht erreichbar: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "MPU6050 aufgeweckt");
    }

    uint8_t buf[14];
    while (1) {
        ret = mpu_read_block(MPU_REG_ACCEL_XOUT_H, buf, sizeof(buf));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Lesefehler: %s", esp_err_to_name(ret));
        } else {
            int16_t ax   = (int16_t)((buf[0]  << 8) | buf[1]);
            int16_t ay   = (int16_t)((buf[2]  << 8) | buf[3]);
            int16_t az   = (int16_t)((buf[4]  << 8) | buf[5]);
            int16_t traw = (int16_t)((buf[6]  << 8) | buf[7]);
            int16_t gx   = (int16_t)((buf[8]  << 8) | buf[9]);
            int16_t gy   = (int16_t)((buf[10] << 8) | buf[11]);
            int16_t gz   = (int16_t)((buf[12] << 8) | buf[13]);

            float temp_c = traw / 340.0f + 36.53f;

            ESP_LOGI(TAG,
                "aX=%6d | aY=%6d | aZ=%6d | tmp=%.2f°C | gX=%6d | gY=%6d | gZ=%6d",
                ax, ay, az, temp_c, gx, gy, gz);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
