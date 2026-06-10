# MiniLingo – Projektfortschritt

> **Lies dieses Dokument zuerst**, wenn du eine neue Session zu diesem
> Projekt startest. Es zeigt, wo wir stehen und was als nächstes ansteht.
>
> - Architektur & Implementierungsplan: [MINILINGO_VOICE_ASSISTANT_SPEC.md](MINILINGO_VOICE_ASSISTANT_SPEC.md)
>   (insb. Section 12 "Implementierungsphasen")
> - Hardware-Validierung der Einzelkomponenten: [COMPONENT_TESTS.md](COMPONENT_TESTS.md)

---

## Status-Übersicht

| Phase | Beschreibung | Status | Notizen/Datum |
|-------|--------------|--------|---------------|
| 0 | Hardware-Komponententests (Mic, Amp, Display, Touch) | ✅ erledigt | Siehe [COMPONENT_TESTS.md](COMPONENT_TESTS.md), 2026-06-10 |
| 1 | Projekt-Skelett + LVGL + UI-Mockup | ✅ erledigt | 2026-06-10, am Gerät verifiziert (UI, Touch, Scrolling) |
| 2 | WiFi + esp-sr + Wake Word | ✅ erledigt | 2026-06-10, Wake Word am Gerät verifiziert (3/3 Detections); WiFi-Code steht, braucht aber noch die 2,4-GHz-SSID |
| 3 | Audio-Capture + Recording-Buffer | ⬜ offen | |
| 4 | MultiNet + Lokale Commands | ⬜ offen | |
| 5 | Groq Whisper Integration | ⬜ offen | |
| 6 | Gemini Flash Integration | ⬜ offen | |
| 7 | Gemini TTS + Wiedergabe | ⬜ offen | |
| 8 | Polish + Robustness | ⬜ offen | |
| 9 (optional) | AEC | ⬜ offen | |
| 10 (optional) | Function-Calling für freie Timer-Werte | ⬜ offen | |

---

## Aktueller Stand / Nächster Schritt

**Stand 2026-06-10 (Session 3):** Phase 2 implementiert, gebaut und
geflasht — Boot-Log zeigt die AFE-Pipeline sauber laufend
(`[input] -> VAD(WebRTC) -> WakeNet(wn9_alexa)`, Threshold 0.8,
Feed-Task mit 512-Sample-Chunks). Aufbau:

- `main/Kconfig.projbuild`: WiFi-Credentials, API-Keys (für spätere
  Phasen), `WAKE_WORD_THRESHOLD` (0–100, Default 80).
- `main/network/wifi.c`: STA-Connect mit Kconfig-Credentials,
  Auto-Reconnect per esp_timer (5s, nicht blockierend im Event-Loop),
  Status-Callback → WiFi-Icon in der Status-Bar.
- esp-sr 2.4.6 als Managed Component; in `sdkconfig.defaults`:
  WakeNet9-Alexa + MultiNet7-EN (schon jetzt mitgeflasht, damit
  Phase 4 kein erneutes Model-Flashen braucht), CPU 240 MHz.
- `main/drivers/audio_in.c` (aus `mic_test.c`): I2S0 RX 16 kHz,
  32→16-bit-Wandlung mit Shift 14 (+12 dB, gesättigt).
- `main/speech/afe_pipeline.c`: AFE-Init + `afe_feed_task`/
  `afe_fetch_task` (Core 1, Prio 5); `main/speech/wake_word.c`:
  Detection-Log + Listener.
- `main.c`: NVS-Init, WiFi-Start, Pipeline-Start; Wake-Word/WiFi-Events
  laufen über atomare Flags + LVGL-Timer in die UI (Button rot,
  State-Text "Listening...", lokale Bubble; Reset nach 3s, bis Phase 3
  echtes Capture bringt). Dummy-Bubbles aus Phase 1 entfernt.

**Acceptance-Test Phase 2 (2026-06-10):** "Alexa" wurde 3/3 Mal
erkannt (Log `Wake word detected`, Ruhe-RMS 10–30, Sprache 250–400).
Wichtig war, den WakeNet-Threshold auf dem **Model-Default (0.64)** zu
belassen — der erste Versuch mit 0.8 erkannte nichts
(`WAKE_WORD_THRESHOLD=0` heißt jetzt "Model-Default verwenden").

**Nächster Schritt:**
1. WiFi: in `idf.py menuconfig` die **2,4-GHz-SSID** eintragen — die
   zuerst eingetragene `HUAWEI-..._5G_Wi-Fi5` ist das 5-GHz-Netz, das
   der ESP32-S3 nicht kann (daher reason 201 = NO_AP_FOUND im Log).
2. Phase 3 starten: Audio-Capture in PSRAM-Recording-Buffer im
   `afe_fetch_task` (Frames nach Wake Word), VAD-Stop nach 1,5s
   Stille / 8s Hard-Limit, `wav.c` mit WAV-Header, Test via
   UART/SPIFFS-Export in Audacity.

**Offene Fragen/Blocker:** nur die 2,4-GHz-WiFi-Credentials (s.o.).
Touch-Feinkalibrierung weiterhin optional.

---

## Wichtige Entscheidungen / Abweichungen vom Spec

- **`LV_COLOR_16_SWAP` existiert in LVGL v9 nicht mehr** (Spec Section 4
  und 14 stammen aus der v8-Welt). Der RGB565-Byte-Swap für den ILI9341
  passiert stattdessen per `lv_draw_sw_rgb565_swap()` im Flush-Callback
  (`main/drivers/display.c`) — so macht es auch das offizielle
  IDF-Beispiel `spi_lcd_touch`.
- **LVGL-Version**: 9.5.0 (Managed Component `lvgl/lvgl ^9`).
- **Mic-Button-Symbol**: LVGL hat kein Mikrofon-Glyph im Default-Font;
  vorerst `LV_SYMBOL_AUDIO` als Platzhalter.
- **Umlaute**: Der Default-Font (Montserrat 14) enthält nur ASCII —
  Dummy-Texte sind umlautfrei. Für echte deutsche Antworten braucht es
  später (Phase 8) einen Custom-Font mit Latin-1-Range.
- **Partitionstabelle** (`model`/`storage`-Partitionen) schon in Phase 1
  angelegt statt erst mit esp-sr/SPIFFS, um spätere Re-Flashes zu
  vermeiden.
- **esp-sr 2.x hat eine andere AFE-API als die Spec** (Section 5 zeigt
  V1-Code): statt `AFE_CONFIG_DEFAULT()` + `ESP_AFE_SR_HANDLE` nun
  `afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST)` +
  `esp_afe_handle_from_config()`; das WakeNet-Model wird automatisch
  aus der model-Partition gewählt. Threshold via
  `set_wakenet_threshold(afe, 1, x)`.
- **MultiNet7-EN schon in Phase 2 in die model-Partition gepackt**
  (srmodels.bin ~3 MB von 5 MB) — geladen wird es erst in Phase 4,
  spart aber ein erneutes Model-Flashen.
- **INMP441-Pegel**: 32→16-bit-Wandlung in `audio_in.c` mit Shift 14
  statt neutral 16 (= +12 dB, mit Sättigung), da das Mic leise ist.
  Tuning-Punkt, falls Wake-Word-Erkennung zu un-/übersensibel ist.

---

## Hardware-Pin-Map

Quelle der Wahrheit ist Spec Section 3 ("Hardware Pin Map"). Falls sich
während der Implementierung Pins ändern: **sowohl hier als auch in der
Spec aktualisieren** (kurzer Verweis + Eintrag oben unter "Wichtige
Entscheidungen").

---

## Konventionen

- Jede Phase wird in eigenen, logisch getrennten Commits abgeschlossen
  (siehe Vorgehen bei den Komponententests: ein Commit pro Test).
- `PROGRESS.md` wird am Ende jeder Session aktualisiert.

---

## ⚠️ Vor Sessionende

Aktualisiere:
1. Die Status-Tabelle oben (Phase erledigt/in Arbeit + Datum).
2. Den Abschnitt "Aktueller Stand / Nächster Schritt".
3. Bei Bedarf: "Wichtige Entscheidungen / Abweichungen vom Spec".
