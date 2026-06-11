#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_client.h"

// Gemeinsame Bausteine fuer die Cloud-Requests (whisper.c, gemini.c):
// alle nutzen das Muster esp_http_client_open + _write + _read.

// esp_http_client_write schreibt ggf. weniger als angefordert.
esp_err_t http_write_all(esp_http_client_handle_t client,
                         const char *data, size_t len);

// Liest den Response-Body (bis buf_len-1 Bytes) und nul-terminiert.
int http_read_body(esp_http_client_handle_t client, char *buf,
                   size_t buf_len);
