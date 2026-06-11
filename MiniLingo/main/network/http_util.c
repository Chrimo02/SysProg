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
