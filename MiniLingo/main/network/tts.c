// Gemini TTS (Spec Section 9.5, Phase 7): responseModalities=AUDIO,
// Response-JSON in PSRAM puffern, inlineData.data per
// mbedtls_base64_decode in den PCM-Buffer dekodieren. Die Sample-Rate
// kommt aus dem mimeType ("audio/L16;codec=pcm;rate=24000").
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#include "config.h"
#include "http_util.h"
#include "tts.h"

static const char *TAG = "tts";

// Preview-Modellname als Konstante (Spec Section 13: kann sich aendern).
#define TTS_MODEL "gemini-2.5-flash-preview-tts"
#define TTS_URL \
    "https://generativelanguage.googleapis.com/v1beta/models/" \
    TTS_MODEL ":generateContent"

// gemini-2.5-flash-preview-tts liefert L16-PCM @ 24 kHz (mimeType
// "audio/L16;codec=pcm;rate=24000"). Das Streaming dekodiert ohne den
// mimeType zu parsen — die Rate ist fuer dieses Modell fix.
#define DEFAULT_RATE 24000

// TTS-Generierung braucht laenger als der Text-Endpoint.
#define TIMEOUT_MS 30000

// Streaming-Puffer: Body chunkweise lesen, Base64 in 4er-Gruppen
// (B64_BATCH ist Vielfaches von 4) inkrementell dekodieren -> PCM.
#define READ_BUF   2048
#define B64_BATCH  4096
#define PCM_BATCH  (B64_BATCH / 4 * 3)  // 3072 Byte

static uint8_t *s_b64;   // Base64-Akku (PSRAM)
static uint8_t *s_pcm;   // dekodiertes PCM eines Batches (PSRAM)

esp_err_t tts_init(void)
{
    s_b64 = heap_caps_malloc(B64_BATCH, MALLOC_CAP_SPIRAM);
    s_pcm = heap_caps_malloc(PCM_BATCH, MALLOC_CAP_SPIRAM);
    if (!s_b64 || !s_pcm) {
        ESP_LOGE(TAG, "PSRAM buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// Request-Body nach Spec Section 9.5; Rueckgabe via cJSON_free().
static char *build_request(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON *turn = cJSON_CreateObject();
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(turn, "parts"), part);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(root, "contents"), turn);

    cJSON *gen = cJSON_AddObjectToObject(root, "generationConfig");
    cJSON *mod = cJSON_AddArrayToObject(gen, "responseModalities");
    cJSON_AddItemToArray(mod, cJSON_CreateString("AUDIO"));
    cJSON *voice = cJSON_AddObjectToObject(
        cJSON_AddObjectToObject(
            cJSON_AddObjectToObject(gen, "speechConfig"), "voiceConfig"),
        "prebuiltVoiceConfig");
    cJSON_AddStringToObject(voice, "voiceName", config_gemini_voice());

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

// Stream-Parser-Zustand fuer einen Response-Body. Findet das
// "data":"-Feld und dekodiert das folgende Base64 chunkweise nach PCM.
typedef struct {
    tts_pcm_cb_t cb;
    void *ctx;
    bool started;       // erster PCM-Chunk an cb gegeben?
    bool done;          // schliessende Quote erreicht
    int match;          // gematchte Zeichen des Key-Tokens
    bool key_done;      // Key "data" gefunden, suche oeffnende Quote
    bool in_data;       // im Base64-Wert
    size_t b64n;        // Fuellstand s_b64
    size_t total;       // ausgegebene Samples (fuer die Logzeile)
} tts_stream_t;

// Nur der Key (inkl. umschliessender Quotes); die oeffnende Quote des
// WERTS folgt nach ":"/Whitespace und wird separat gesucht — so ist der
// Parser unabhaengig davon, ob die JSON-Antwort kompakt ("data":") oder
// pretty-printed ("data": ") ist (Gemini liefert mit Leerzeichen).
static const char DATA_TOKEN[] = "\"data\"";
#define DATA_TOKEN_LEN (sizeof(DATA_TOKEN) - 1)

// Einen Base64-Batch dekodieren und an den Callback geben. n muss ein
// Vielfaches von 4 sein (gilt fuer volle Batches und — da gueltiges
// Base64 4-Byte-aligned ist — auch fuer den Rest am Ende).
static esp_err_t flush_batch(tts_stream_t *s, size_t n)
{
    if (n == 0) {
        return ESP_OK;
    }
    size_t olen = 0;
    int err = mbedtls_base64_decode(s_pcm, PCM_BATCH, &olen, s_b64, n);
    if (err != 0) {
        ESP_LOGE(TAG, "base64 decode failed (-0x%04x)", -err);
        return ESP_FAIL;
    }
    if (olen == 0) {
        return ESP_OK;
    }
    s->started = true;
    s->total += olen / sizeof(int16_t);
    return s->cb((int16_t *)s_pcm, olen / sizeof(int16_t), DEFAULT_RATE,
                 s->ctx);
}

// Ein Body-Byte durch den Parser schicken.
static esp_err_t feed_byte(tts_stream_t *s, char c)
{
    if (!s->in_data) {
        if (!s->key_done) {
            // Key "data" suchen (naiver Matcher; kommt im Gemini-Response
            // genau einmal als Audio-Feld vor — "inlineData"/"mimeType"
            // matchen nicht).
            if (c == DATA_TOKEN[s->match]) {
                if (++s->match == (int)DATA_TOKEN_LEN) {
                    s->key_done = true;
                }
            } else {
                s->match = (c == DATA_TOKEN[0]) ? 1 : 0;
            }
            return ESP_OK;
        }
        // Key gefunden: bis zur oeffnenden Quote des Werts springen
        // (dazwischen nur ":"/Whitespace, kein weiteres ").
        if (c == '"') {
            s->in_data = true;
            s->b64n = 0;
        }
        return ESP_OK;
    }
    if (c == '"') {                       // Ende des Base64-Werts (das
                                          // Base64-Alphabet enthaelt kein ")
        esp_err_t r = flush_batch(s, s->b64n);
        s->b64n = 0;
        s->done = true;
        return r;
    }
    s_b64[s->b64n++] = (uint8_t)c;
    if (s->b64n == B64_BATCH) {
        esp_err_t r = flush_batch(s, B64_BATCH);
        s->b64n = 0;
        return r;
    }
    return ESP_OK;
}

static esp_err_t synthesize_stream_once(const char *text, tts_pcm_cb_t cb,
                                        void *ctx, bool *started)
{
    *started = false;
    if (strlen(config_gemini_key()) == 0) {
        ESP_LOGE(TAG, "Gemini API key not set (Setup-Portal/menuconfig)");
        return ESP_ERR_INVALID_STATE;
    }

    char *body = build_request(text);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = TTS_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = 8192,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        cJSON_free(body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "x-goog-api-key",
                               config_gemini_key());
    esp_http_client_set_header(client, "Content-Type",
                               "application/json");

    esp_err_t err = esp_http_client_open(client, (int)strlen(body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        cJSON_free(body);
        return err;
    }

    int64_t t0 = esp_log_timestamp();
    err = http_write_all(client, body, strlen(body));
    cJSON_free(body);

    int status = -1;
    if (err == ESP_OK) {
        if (esp_http_client_fetch_headers(client) < 0) {
            ESP_LOGE(TAG, "no response headers");
            err = ESP_FAIL;
        } else {
            status = esp_http_client_get_status_code(client);
        }
    }

    if (err == ESP_OK && status != 200) {
        // Fehlerkoerper kurz fuer die Logzeile lesen. 429 = Free-Tier-
        // Tageslimit (eigene Kennung); 5xx transient (Preview wirft
        // sporadisch 500), restliche 4xx permanent.
        char errbuf[256];
        int n = esp_http_client_read(client, errbuf, sizeof(errbuf) - 1);
        errbuf[n > 0 ? n : 0] = '\0';
        ESP_LOGE(TAG, "HTTP %d: %s", status, errbuf);
        err = (status == 429)  ? ESP_ERR_NOT_ALLOWED
              : (status >= 500) ? ESP_FAIL
                                : ESP_ERR_INVALID_RESPONSE;
    } else if (err == ESP_OK) {
        // 200: Body streamen + chunkweise dekodieren/ausgeben.
        tts_stream_t st = { .cb = cb, .ctx = ctx };
        char rbuf[READ_BUF];
        int n;
        while ((n = esp_http_client_read(client, rbuf, sizeof(rbuf))) > 0) {
            for (int i = 0; i < n && !st.done; i++) {
                err = feed_byte(&st, rbuf[i]);
                if (err != ESP_OK) {
                    break;  // cb-Abbruch ("stop") oder Decode-Fehler
                }
            }
            if (st.done || err != ESP_OK) {
                break;
            }
        }
        *started = st.started;
        if (err == ESP_OK && !st.done) {
            // 200, aber kein Audio (z.B. finishReason=OTHER) — transient.
            ESP_LOGE(TAG, "no audio in response");
            err = ESP_FAIL;
        } else if (err == ESP_OK) {
            ESP_LOGI(TAG, "audio streamed in %u ms (%.1f s @ %u Hz)",
                     (unsigned)(esp_log_timestamp() - t0),
                     (float)st.total / (float)DEFAULT_RATE,
                     (unsigned)DEFAULT_RATE);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t tts_synthesize_stream(const char *text, tts_pcm_cb_t cb, void *ctx)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < HTTP_MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            http_backoff_delay(attempt, TAG);
        }
        bool started = false;
        err = synthesize_stream_once(text, cb, ctx, &started);
        // Sobald Audio lief, nicht erneut versuchen (schon abgespielt).
        if (started || !http_err_is_transient(err)) {
            break;
        }
    }
    return err;
}
