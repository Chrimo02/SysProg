// WLAN-Setup-Portal (SoftAP + HTTP). Trigger: Tap aufs WiFi-Icon
// (ui_status_bar) -> provisioning_start(). Das Gerät bleibt im APSTA-
// Modus (die bestehende STA-Verbindung läuft weiter), spannt zusätzlich
// den AP "MiniLingo-Setup" auf und serviert ein Formular. POST /save
// schreibt die Werte ins NVS (config.c) und rebootet.
//
// Sicherheit: WPA2-geschützter AP, Werte im Klartext im NVS — für dieses
// Hobby-Setup akzeptabel (Kconfig war auch Klartext). Kein TLS auf dem
// lokalen Portal.
#include <stdatomic.h>              // [C] Header-Include | [C] atomare Typen/Operationen (atomic_bool, atomic_load/exchange) für threadsicheren Zustand
#include <stdio.h>                  // [C] Header-Include | [C] snprintf für den formatierten HTML-Aufbau
#include <string.h>                 // [C] Header-Include | [C] strlen für die SSID-Länge

#include "freertos/FreeRTOS.h"      // [C] Header-Include | [HW] FreeRTOS-Basistypen/Makros, u.a. pdMS_TO_TICKS
#include "freertos/task.h"          // [C] Header-Include | [HW] Task-API (xTaskCreate, vTaskDelay) für den Reboot-Task

#include "esp_http_server.h"        // [HW] ESP-IDF HTTP-Server-API: httpd_start, URI-Handler, Request/Response
#include "esp_log.h"                // [HW] ESP-IDF Logging-Makros (ESP_LOGI/ESP_LOGE)
#include "esp_netif.h"               // [HW] ESP-IDF Netzwerk-Interface-Abstraktion (u.a. esp_netif_create_default_wifi_ap)
#include "esp_system.h"              // [HW] Systemfunktionen, u.a. esp_restart() für den Reboot
#include "esp_wifi.h"                // [HW] WiFi-Treiber-API: Moduswechsel, AP-Konfiguration

#include "config.h"                  // [Logik] Zugriff auf die persistente Konfiguration (NVS-Wrapper, CFG_KEY_*)
#include "provisioning.h"            // [C] eigenes Modul-Header-Include | [Logik] öffentliche Deklarationen dieser Datei

static const char *TAG = "provision";  // [C] static = Datei-lokale (interne) Linkage | [Logik] Log-Tag zur Zuordnung der Log-Ausgaben

#define AP_SSID     "MiniLingo-Setup"   // [C] Präprozessor-Konstante (Textersetzung) | [HW] SSID, unter der das Setup-Portal im WLAN sichtbar ist
#define AP_PASS     "minilingo"  // >= 8 Zeichen für WPA2  // [C] Präprozessor-Konstante | [HW] WPA2-Pre-Shared-Key des SoftAP, Mindestlänge 8 Zeichen laut WPA2-Spezifikation
#define AP_MAX_CONN 2                    // [C] Präprozessor-Konstante | [HW] maximale Anzahl gleichzeitig verbundener Stationen am SoftAP

static atomic_bool s_active;      // [C] static = Datei-lokale Variable | [C] atomarer bool-Typ, threadsicher ohne separaten Mutex | [Logik] hält fest, ob das Portal bereits läuft
static httpd_handle_t s_server;   // [C] static Zeigervariable | [HW] Handle auf die laufende HTTP-Server-Instanz
static esp_netif_t *s_ap_netif;    // [C] static Zeiger auf esp_netif_t | [HW] Netzwerk-Interface-Objekt des SoftAP

// --- URL-Decoding (application/x-www-form-urlencoded) -----------------

// Wandelt ein einzelnes Hex-Zeichen ('0'-'9', 'a'-'f', 'A'-'F') in seinen
// Zahlenwert (0-15) um. Wird für das Dekodieren von %XX-Escapes in
// URL-kodierten Formulardaten gebraucht. Liefert -1 bei ungültigem Zeichen.
static int hexval(char c)          // [C] static = interne Linkage, Rückgabetyp int | [Logik] Hilfsfunktion für URL-Dekodierung
{
    if (c >= '0' && c <= '9') return c - '0';        // [C] Vergleichsoperatoren + Zeichenarithmetik | [Logik] Ziffer 0-9 erkennen und Zahlenwert berechnen
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;    // [C] Zeichenarithmetik | [Logik] Kleinbuchstabe a-f (Hex 10-15)
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;    // [C] Zeichenarithmetik | [Logik] Großbuchstabe A-F (Hex 10-15)
    return -1;                                        // [Logik] kein gültiges Hex-Zeichen -> Fehlerwert
}

// Dekodiert einen application/x-www-form-urlencoded-String in-place:
// %XX-Escapes werden zu ihrem Byte-Wert, '+' wird zu Leerzeichen. Wird auf
// jedes vom HTTP-Server gelesene Formularfeld angewendet, bevor es ins
// NVS geschrieben wird.
static void url_decode(char *s)    // [C] static, Parameter ist Zeiger auf char (String), Rückgabe void | [Logik] In-place-Dekodierung eines URL-kodierten Strings
{
    char *d = s;                   // [C] zweiter Zeiger als Schreib-Cursor, s bleibt Lese-Cursor | [Logik] ermöglicht In-place-Kompaktierung (Ausgabe kann kürzer sein als Eingabe)
    while (*s) {                   // [C] Dereferenzierung + implizite Null-Terminator-Prüfung | [Logik] Schleife bis zum String-Ende
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {  // [C] Array-Indexierung auf Zeiger, Kurzschluss-Auswertung && | [HW] erkennt ein %XX-Escape gemäß URL-Encoding
            *d++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));  // [C] Cast zu char, Post-Inkrement des Schreibzeigers | [Logik] zwei Hex-Ziffern zu einem Byte zusammensetzen
            s += 3;                 // [C] Zeigerarithmetik | [Logik] '%' + 2 Hex-Ziffern überspringen
        } else if (*s == '+') {    // [C] Vergleich auf dereferenziertes Zeichen | [HW] '+' steht im Form-Encoding für Leerzeichen
            *d++ = ' ';             // [C] Schreibzeiger inkrementieren nach Zuweisung | [Logik] '+' durch Leerzeichen ersetzen
            s++;                    // [C] Zeigerarithmetik | [Logik] Lesezeiger weiterschieben
        } else {
            *d++ = *s++;            // [C] gleichzeitiges Kopieren und Inkrementieren beider Zeiger | [Logik] unverändertes Zeichen übernehmen
        }
    }
    *d = '\0';                      // [C] Null-Terminierung des Ergebnis-Strings | [Logik] String nach dem Dekodieren korrekt abschließen
}

// Ein Formularfeld aus dem Body ziehen, decoden und ins NVS schreiben.
// skip_if_empty: leere Geheimfelder (Passwort/Keys) lassen den alten
// Wert stehen.
static void apply_field(const char *body, const char *field,
                        const char *cfg_key, bool skip_if_empty)  // [C] static Funktion, mehrere const-char*-Parameter + bool-Flag | [Logik] generische Übernahme eines Formularfelds in die Konfiguration
{
    char val[160];                 // [C] Stack-Array fester Größe als Zwischenpuffer | [Logik] Aufnahme des dekodierten Feldwerts
    if (httpd_query_key_value(body, field, val, sizeof(val)) != ESP_OK) {  // [HW] ESP-IDF-HTTP-Server-API extrahiert Wert zu "field" aus dem Body | [Logik] Feld nicht vorhanden -> nichts tun
        return;                     // [Logik] frühzeitiger Ausstieg bei fehlendem Feld
    }
    url_decode(val);                // [Logik] rohen Formularwert in lesbaren Klartext dekodieren
    if (skip_if_empty && val[0] == '\0') {  // [C] Zugriff auf erstes Array-Element, Vergleich mit Null-Terminator | [Logik] Geheimfeld leer gelassen -> alten Wert im NVS nicht überschreiben
        return;                     // [Logik] frühzeitiger Ausstieg, altes Passwort/Key bleibt bestehen
    }
    config_set_str(cfg_key, val);   // [HW] Schreibt den Wert persistent ins NVS über den config-Wrapper | [Logik] eigentliche Übernahme der Nutzereingabe
}

// --- HTTP-Handler -----------------------------------------------------

// HTTP-GET-Handler für "/": liefert das Setup-Formular als HTML aus,
// vorbefüllt mit den aktuell gespeicherten (nicht-geheimen) Werten.
// Wird vom HTTP-Server bei jedem Aufruf der Startseite ausgeführt.
static esp_err_t get_handler(httpd_req_t *req)  // [C] Rückgabetyp esp_err_t, Parameter ist Zeiger auf Request-Struct | [HW] Callback-Funktionszeiger-Signatur, wie sie httpd_uri_t erwartet
{
    static char html[2048];         // [C] static = Puffer bleibt über Aufrufe hinweg im BSS/Daten-Segment statt auf dem Stack | [Logik] großer HTML-Puffer, spart Stack-Verbrauch
    snprintf(html, sizeof(html),    // [C] snprintf begrenzt Schreibvorgang auf Puffergröße, verhindert Buffer-Overflow | [Logik] HTML-Seite mit aktuellen Werten zusammenbauen
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>MiniLingo Setup</title>"
        "<style>body{font-family:sans-serif;max-width:480px;margin:1em auto;"
        "padding:0 1em}label{display:block;margin:.6em 0 .2em;font-weight:bold}"
        "input{width:100%%;padding:.5em;box-sizing:border-box}"
        "button{margin-top:1em;padding:.7em 1.4em;font-size:1em}"
        "small{color:#666}</style></head><body>"
        "<h2>MiniLingo Setup</h2>"
        "<form method=post action=/save>"
        "<label>WLAN-SSID</label><input name=ssid value='%s'>"
        "<label>WLAN-Passwort</label>"
        "<input name=pass type=password placeholder='unveraendert lassen'>"
        "<label>Cerebras API-Key (LLM)</label>"
        "<input name=cerebras type=password placeholder='unveraendert lassen'>"
        "<label>Groq API-Key (Whisper)</label>"
        "<input name=groq type=password placeholder='unveraendert lassen'>"
        "<label>Gemini API-Key (TTS)</label>"
        "<input name=gemini type=password placeholder='unveraendert lassen'>"
        "<label>LLM-Modell</label><input name=model value='%s'>"
        "<label>Whisper-Sprache</label><input name=lang value='%s'>"
        "<label>Gemini-Voice</label><input name=voice value='%s'>"
        "<button type=submit>Speichern &amp; Neustart</button>"
        "<p><small>Geheimfelder leer lassen, um den bisherigen Wert zu "
        "behalten.</small></p>"
        "</form></body></html>",
        config_wifi_ssid(), config_llm_model(), config_whisper_language(),  // [Logik] aktuelle Konfigurationswerte aus config.c einsetzen (Passwörter/Keys bewusst NICHT eingesetzt)
        config_gemini_voice());

    httpd_resp_set_type(req, "text/html; charset=utf-8");  // [HW] setzt den HTTP-Content-Type-Header der Antwort
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);  // [HW] sendet die HTML-Antwort; HTTPD_RESP_USE_STRLEN lässt den Server die Länge per strlen bestimmen | [Logik] Rückgabe des esp_err_t-Ergebnisses an den HTTP-Server
}

// Hintergrund-Task, der kurz wartet und dann das Gerät neu startet.
// Läuft entkoppelt vom HTTP-Request-Handler, damit die "Gespeichert"-
// Antwort noch beim Client ankommt, bevor der Neustart erfolgt.
static void reboot_task(void *arg)  // [C] Task-Funktionssignatur (void*-Parameter, kein Rückgabewert), passend zu xTaskCreate | [HW] FreeRTOS-Task-Callback
{
    vTaskDelay(pdMS_TO_TICKS(1500));  // [HW] FreeRTOS-Delay, 1500ms in Ticks umgerechnet | [Logik] Wartezeit, damit die HTTP-Antwort sicher noch rausgeht
    esp_restart();                    // [HW] löst einen Soft-Reset des ESP32 aus | [Logik] übernimmt die neue Konfiguration nach dem Neustart
}

// HTTP-POST-Handler für "/save": liest den Formular-Body, übernimmt die
// Felder ins NVS (config.c), schickt eine Bestätigungsseite und stößt
// per separatem Task den Neustart des Geräts an.
static esp_err_t save_handler(httpd_req_t *req)  // [C] Rückgabetyp esp_err_t, Parameter Zeiger auf Request | [HW] Callback-Signatur für httpd_uri_t
{
    char body[1024];                // [C] Stack-Puffer für den POST-Body | [Logik] Aufnahme der rohen Formulardaten
    int len = req->content_len;     // [C] Zugriff auf Struct-Feld über Pfeiloperator | [HW] vom Client gemeldete Content-Length des Requests
    if (len <= 0 || len >= (int)sizeof(body)) {  // [C] Cast von size_t auf int für den Vergleich, Kurzschluss-Oder || | [Logik] Plausibilitätsprüfung: leer oder zu groß für den Puffer
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");  // [HW] sendet HTTP 400 als Fehlerantwort
        return ESP_FAIL;             // [Logik] Abbruch bei ungültiger Anfrage
    }
    int got = httpd_req_recv(req, body, len);  // [HW] liest den Request-Body vom Socket in den Puffer | [Logik] tatsächlich empfangene Bytes merken
    if (got <= 0) {                  // [C] Vergleichsoperator | [Logik] Empfang fehlgeschlagen oder Verbindung geschlossen
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");  // [HW] HTTP-400-Fehlerantwort senden
        return ESP_FAIL;             // [Logik] Abbruch bei Empfangsfehler
    }
    body[got] = '\0';                // [C] Array-Indexzugriff, Null-Terminierung | [Logik] Body als gültigen C-String nutzbar machen (recv liefert keinen Terminator)

    // Nicht-Geheim: immer übernehmen (Felder sind vorbefüllt). Geheim:
    // nur wenn ausgefüllt.
    apply_field(body, "ssid",     CFG_KEY_WIFI_SSID,    false);  // [HW] übernimmt die neue WLAN-SSID ins NVS | [Logik] false = auch leerer Wert würde übernommen (Feld ist aber vorbefüllt)
    apply_field(body, "model",    CFG_KEY_LLM_MODEL,    false);  // [Logik] LLM-Modellname übernehmen
    apply_field(body, "lang",     CFG_KEY_WHISPER_LANG, false);  // [Logik] Whisper-Spracheinstellung übernehmen
    apply_field(body, "voice",    CFG_KEY_GEMINI_VOICE, false);  // [Logik] Gemini-Voice-Einstellung übernehmen
    apply_field(body, "pass",     CFG_KEY_WIFI_PASS,    true);   // [HW] WLAN-Passwort nur überschreiben, wenn ausgefüllt | [Logik] true = leeres Feld lässt bestehendes Passwort unverändert
    apply_field(body, "cerebras", CFG_KEY_CEREBRAS_KEY, true);   // [Logik] Cerebras-API-Key nur bei Eingabe übernehmen
    apply_field(body, "groq",     CFG_KEY_GROQ_KEY,     true);   // [Logik] Groq-API-Key nur bei Eingabe übernehmen
    apply_field(body, "gemini",   CFG_KEY_GEMINI_KEY,   true);   // [Logik] Gemini-API-Key nur bei Eingabe übernehmen

    ESP_LOGI(TAG, "config saved, rebooting");  // [HW] Log-Ausgabe über ESP-IDF-Logging
    httpd_resp_set_type(req, "text/html; charset=utf-8");  // [HW] Content-Type-Header für die Bestätigungsseite setzen
    httpd_resp_sendstr(req,                     // [HW] sendet einen Null-terminierten String als HTTP-Antwort
        "<html><body style='font-family:sans-serif'>"
        "<h2>Gespeichert</h2><p>MiniLingo startet neu...</p>"
        "</body></html>");

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);  // [C] übergibt reboot_task als Funktionszeiger | [HW] FreeRTOS-Task-Erzeugung: 2048 Byte Stack, Priorität 5, kein Task-Handle benötigt | [Logik] Reboot asynchron anstoßen, damit die Antwort noch gesendet werden kann
    return ESP_OK;                   // [Logik] Handler erfolgreich abgeschlossen
}

// --- Start ------------------------------------------------------------

// Konfiguriert und aktiviert den SoftAP "MiniLingo-Setup" im reinen
// AP-Modus. Legt bei Bedarf das Netzwerk-Interface an und setzt WiFi in
// den AP-Modus, damit das Setup-Portal per WLAN erreichbar ist.
static void start_softap(void)      // [C] static Funktion, kein Parameter, kein Rückgabewert | [HW] richtet den SoftAP für das Provisioning-Portal ein
{
    if (!s_ap_netif) {               // [C] Negation eines Zeigers (NULL-Prüfung) | [Logik] Netif nur einmal anlegen, nicht bei jedem Start neu
        s_ap_netif = esp_netif_create_default_wifi_ap();  // [HW] erstellt das Standard-WiFi-AP-Netzwerkinterface der ESP-IDF-Netif-Schicht
    }

    wifi_config_t ap_cfg = {         // [C] Struct-Initialisierung mit designated initializers | [HW] WiFi-Konfigurationsstruktur für den AP-Modus
        .ap = {                      // [C] Zugriff auf die "ap"-Union-Variante von wifi_config_t
            .ssid = AP_SSID,         // [HW] Netzwerkname des SoftAP
            .ssid_len = strlen(AP_SSID),  // [C] Laufzeitberechnung der Stringlänge | [HW] explizite SSID-Länge für den WiFi-Treiber
            .password = AP_PASS,     // [HW] WPA2-Passwort des SoftAP
            .channel = 1,            // [HW] fester WiFi-Kanal 1 für den AP
            .max_connection = AP_MAX_CONN,  // [HW] Begrenzung gleichzeitiger Client-Verbindungen
            .authmode = WIFI_AUTH_WPA2_PSK,  // [HW] Authentifizierungsverfahren: WPA2 mit Pre-Shared-Key
        },
    };
    // Reiner AP-Modus (STA aus): der ESP hat nur EIN Funkmodul — bei
    // aktiver Heim-WLAN-Verbindung (APSTA) muss der AP auf dessen Kanal
    // und kickt den Setup-Client (join/leave reason 1). Fuer das Setup
    // braucht der AP das Funkmodul allein; nach dem Speichern rebootet
    // das Geraet ohnehin und die STA-Verbindung kommt wieder.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));  // [C] Makro, das bei Fehler abbricht (asserts intern) | [HW] schaltet den WiFi-Treiber in den reinen AP-Modus
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));  // [C] Übergabe der Struct-Adresse per Zeiger | [HW] wendet die AP-Konfiguration auf das AP-Interface an
    ESP_LOGI(TAG, "SoftAP \"%s\" up (AP-only, ch1) — http://192.168.4.1",  // [HW] Log-Ausgabe mit eingebetteter SSID
             AP_SSID);
}

// Startet den ESP-IDF-HTTP-Server und registriert die beiden Endpunkte
// "/" (Formular anzeigen) und "/save" (Formular speichern). Wird von
// provisioning_start() nach dem SoftAP-Aufbau aufgerufen.
static void start_httpd(void)       // [C] static Funktion, kein Parameter/Rückgabewert | [HW] initialisiert den HTTP-Server samt URI-Routing
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();  // [C] Makro-Expansion liefert eine vorbelegte Konfigurationsstruktur | [HW] Standard-HTTP-Server-Konfiguration der ESP-IDF
    if (httpd_start(&s_server, &cfg) != ESP_OK) {  // [C] Übergabe von Zeigern auf Handle und Konfiguration | [HW] startet den HTTP-Server-Task
        ESP_LOGE(TAG, "httpd_start failed");        // [HW] Fehler-Log bei gescheitertem Serverstart
        return;                                     // [Logik] Abbruch, keine Handler registrieren wenn Server nicht läuft
    }
    httpd_uri_t get_uri = {          // [C] Struct-Initialisierung mit designated initializers | [HW] Routing-Eintrag für GET "/"
        .uri = "/", .method = HTTP_GET, .handler = get_handler };  // [C] handler ist ein Funktionszeiger auf get_handler | [HW] verknüpft GET "/" mit der Formularanzeige
    httpd_uri_t save_uri = {         // [C] Struct-Initialisierung | [HW] Routing-Eintrag für POST "/save"
        .uri = "/save", .method = HTTP_POST, .handler = save_handler };  // [C] handler-Funktionszeiger auf save_handler | [HW] verknüpft POST "/save" mit dem Speicher-Handler
    httpd_register_uri_handler(s_server, &get_uri);   // [HW] registriert den GET-Handler beim laufenden Server
    httpd_register_uri_handler(s_server, &save_uri);  // [HW] registriert den POST-Handler beim laufenden Server
}

// Öffentliche Funktion: startet das Provisioning-Portal (SoftAP + HTTP-
// Server) genau einmal. Weitere Aufrufe, während es bereits läuft,
// werden ignoriert (idempotent), damit z.B. mehrfaches Tippen auf das
// WiFi-Icon keinen doppelten Server erzeugt.
void provisioning_start(void)        // [C] öffentliche (extern) Funktion ohne Parameter/Rückgabewert | [Logik] zentraler Einstiegspunkt zum Starten des Setup-Portals
{
    if (atomic_exchange(&s_active, true)) {  // [C] atomic_exchange setzt neuen Wert und liefert den alten atomar zurück | [Logik] verhindert Race Conditions bei gleichzeitigem Start aus mehreren Tasks
        return;  // schon aktiv               // [Logik] Portal läuft bereits -> nichts weiter tun
    }
    start_softap();                  // [HW] SoftAP für das Setup-Portal aufbauen
    start_httpd();                   // [HW] HTTP-Server mit den Formular-Endpunkten starten
}

// Öffentliche Funktion: gibt zurück, ob das Provisioning-Portal aktuell
// aktiv ist. Wird z.B. von der UI genutzt, um den Aktivierungsstatus
// anzuzeigen, ohne den internen atomaren Zustand direkt anzufassen.
bool provisioning_is_active(void)    // [C] öffentliche Funktion, Rückgabetyp bool | [Logik] Statusabfrage für Aufrufer außerhalb dieses Moduls
{
    return atomic_load(&s_active);   // [C] atomic_load liest den Wert threadsicher | [Logik] aktuellen Aktivierungszustand zurückgeben
}
