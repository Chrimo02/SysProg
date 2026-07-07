#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "http_util.h"

static const char *TAG = "http";

esp_err_t http_write_all(esp_http_client_handle_t client,
                         const char *data, size_t len)
{
    while (len > 0) {
        int n = esp_http_client_write(client, data, (int)len);
        if (n <= 0) {
            ESP_LOGE(TAG, "upload write failed");
            return ESP_FAIL;
        }
        data += n;
        len -= (size_t)n;
    }
    return ESP_OK;
}

int http_read_body(esp_http_client_handle_t client, char *buf,
                   size_t buf_len)
{
    size_t total = 0;
    while (total < buf_len - 1) {
        int n = esp_http_client_read(client, buf + total,
                                     (int)(buf_len - 1 - total));
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    buf[total] = '\0';
    return (int)total;
}

bool http_err_is_transient(esp_err_t err)
{
    return err != ESP_OK && err != ESP_ERR_INVALID_STATE &&
           err != ESP_ERR_INVALID_RESPONSE && err != ESP_ERR_NOT_ALLOWED;
}

void http_backoff_delay(int attempt, const char *tag)
{
    uint32_t ms = 1000u << (attempt - 1);
    ESP_LOGW(tag, "retrying in %u ms (attempt %d/%d)", (unsigned)ms,
             attempt + 1, HTTP_MAX_ATTEMPTS);
    vTaskDelay(pdMS_TO_TICKS(ms));
}
