#include "freertos/FreeRTOS.h"      // [C] Header-Include | [HW] FreeRTOS-Basistypen/Makros, u.a. pdMS_TO_TICKS für Zeitumrechnung
#include "freertos/task.h"          // [C] Header-Include | [HW] deklariert vTaskDelay für blockierendes Warten im Task

#include "esp_log.h"                // [HW] ESP-IDF-Logging-API (ESP_LOGE/ESP_LOGW) mit Tag-basierter Ausgabe

#include "http_util.h"              // [C] eigenes Modul-Header-Include | [Logik] eigene Deklarationen und HTTP_MAX_ATTEMPTS

static const char *TAG = "http";    // [C] static = Datei-lokale (interne) Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben dieses Moduls

// Schreibt einen vollständigen Datenpuffer über einen offenen HTTP-Client.
// esp_http_client_write kann pro Aufruf weniger Bytes annehmen als angefragt
// (z. B. bei vollem Socket-Sendepuffer), daher wird in einer Schleife
// nachgeschrieben, bis alles gesendet ist oder ein Fehler auftritt.
esp_err_t http_write_all(esp_http_client_handle_t client,   // [C] Rückgabetyp esp_err_t (Fehlercode) | [HW] Handle der offenen HTTP-Verbindung
                         const char *data, size_t len)       // [C] const-Zeiger auf zu sendende Bytes und deren Gesamtlänge
{
    while (len > 0) {                                                  // [C] Schleife läuft, solange noch unversendete Bytes übrig sind | [Logik] Abbruchbedingung: alles geschrieben
        int n = esp_http_client_write(client, data, (int)len);         // [C] Cast size_t -> int für die API-Signatur | [HW] schreibt (Teil-)Daten auf den HTTP-Request-Body/Socket
        if (n <= 0) {                                                  // [C] Vergleichsoperator, negative/0-Werte signalisieren Fehler | [Logik] Schreibfehler erkannt -> Abbruch
            ESP_LOGE(TAG, "upload write failed");                      // [HW] Fehler-Log über esp_log mit Modul-Tag
            return ESP_FAIL;                                           // [Logik] Fehler an Aufrufer melden
        }
        data += n;                                                     // [C] Zeigerarithmetik: Datenzeiger um geschriebene Bytes vorrücken
        len -= (size_t)n;                                              // [C] Cast int -> size_t | [Logik] Restlänge um geschriebene Bytes verringern
    }
    return ESP_OK;                                                    // [Logik] alle Bytes erfolgreich geschrieben
}

// Liest den kompletten Response-Body einer HTTP-Antwort in einen Puffer und
// nul-terminiert ihn, damit er anschließend als C-String (z. B. für
// JSON-Parsing) verwendet werden kann. Liest maximal buf_len-1 Bytes, um
// Platz für den Null-Terminator zu reservieren.
int http_read_body(esp_http_client_handle_t client, char *buf,   // [C] Rückgabetyp int (Anzahl gelesener Bytes) | [HW] client = HTTP-Handle, buf = Zielpuffer
                   size_t buf_len)                                // [C] Gesamtgröße des Zielpuffers inklusive Terminator-Byte
{
    size_t total = 0;                                                       // [C] Zähler für bereits gelesene Bytes, Initialisierung mit 0 | [Logik] Fortschritt des Lesevorgangs
    while (total < buf_len - 1) {                                          // [C] Schleifenbedingung, buf_len-1 lässt Platz für '\0' | [Logik] läuft bis Puffer (fast) voll ist
        int n = esp_http_client_read(client, buf + total,                  // [C] Zeigerarithmetik: Schreibposition im Puffer nach bisherigem Fortschritt | [HW] liest Bytes aus dem HTTP-Response-Body
                                     (int)(buf_len - 1 - total));           // [C] Cast size_t -> int, verbleibende Pufferkapazität als Lesegrenze
        if (n <= 0) {                                                      // [C] Vergleichsoperator | [Logik] kein Byte mehr gelesen -> Verbindungsende oder Fehler
            break;                                                         // [Logik] Leseschleife beenden
        }
        total += (size_t)n;                                                // [C] Cast int -> size_t | [Logik] Gesamtzähler um gelesene Bytes erhöhen
    }
    buf[total] = '\0';                                                     // [C] Array-Indexzugriff, schreibt Null-Terminator ans Ende der gelesenen Daten | [Logik] macht Puffer zu gültigem C-String
    return (int)total;                                                     // [C] Cast size_t -> int | [Logik] gibt Anzahl gelesener Nutzdaten-Bytes zurück
}

// Klassifiziert einen esp_err_t-Fehlercode als transient (wiederholbar durch
// Retry) oder permanent. Wird von der Retry-Logik der Cloud-Requests
// (whisper.c, gemini.c) genutzt, um zu entscheiden, ob ein erneuter Versuch
// sinnvoll ist.
bool http_err_is_transient(esp_err_t err)   // [C] Rückgabetyp bool | [HW] err = zu prüfender Fehlercode eines HTTP-Requests
{
    return err != ESP_OK && err != ESP_ERR_INVALID_STATE &&               // [C] logische UND-Verknüpfung mehrerer Ungleichheitsvergleiche | [Logik] kein Fehler und bekannte Permanentfehler ausschließen
           err != ESP_ERR_INVALID_RESPONSE && err != ESP_ERR_NOT_ALLOWED;  // [Logik] weitere Permanentfehler (4xx außer 429, TTS-Tageslimit) ausschließen -> Rest gilt als transient
}

// Wartet vor dem nächsten Retry-Versuch eine exponentiell steigende Zeit
// (1 s, 2 s, ...) und protokolliert den Versuch. Verhindert, dass
// aufeinanderfolgende Fehlversuche Server oder Netz sofort erneut belasten.
void http_backoff_delay(int attempt, const char *tag)   // [C] kein Rückgabewert | [Logik] attempt = aktuelle Versuchsnummer (ab 1), tag = Log-Tag des Aufrufers
{
    uint32_t ms = 1000u << (attempt - 1);                                  // [C] Bit-Shift-Operator realisiert Verdopplung pro Versuch (exponentieller Backoff) | [Logik] 1000ms, 2000ms, ...
    ESP_LOGW(tag, "retrying in %u ms (attempt %d/%d)", (unsigned)ms,       // [C] Cast uint32_t -> unsigned für printf-Formatstring | [HW] Warn-Log über esp_log mit aufruferspezifischem Tag
             attempt + 1, HTTP_MAX_ATTEMPTS);                              // [Logik] zeigt kommenden Versuch (attempt+1) von HTTP_MAX_ATTEMPTS an
    vTaskDelay(pdMS_TO_TICKS(ms));                                         // [C] Makro-Expansion rechnet Millisekunden in FreeRTOS-Ticks um | [HW] blockiert den aufrufenden Task für die Backoff-Dauer
}
