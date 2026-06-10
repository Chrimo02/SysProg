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
| 1 | Projekt-Skelett + LVGL + UI-Mockup | 🟡 implementiert | 2026-06-10, baut sauber; Hardware-Test (Acceptance) steht aus |
| 2 | WiFi + esp-sr + Wake Word | ⬜ offen | |
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

**Stand 2026-06-10 (Session 2):** Phase 1 implementiert und der Build
läuft fehlerfrei durch (`idf.py build`, App ~520 KB). Neu:

- Projektstruktur gemäß Spec Section 11: `main/main.c`,
  `main/drivers/` (display, touch), `main/ui/` (ui, ui_status_bar,
  ui_chat, ui_mic_button). Die `*_test.c`-Dateien bleiben als Referenz
  in `main/`, werden aber nicht mitgebaut.
- LVGL 9.5.0 + `esp_lcd_ili9341` als Managed Components,
  `sdkconfig.defaults` (PSRAM Octal, 16 MB Flash, Custom-Partitions)
  und `partitions.csv` (nvs/factory/model/storage) angelegt.
- UI: Status-Bar (WiFi/State/Volume), scrollbarer Chat-Container mit
  Bubble-Helpern (User blau rechts, Assistant grau links, Local grün
  links), Mic-Button mit Zustandsfarben + Klick-Log und Dummy-Bubbles.

**Nächster Schritt:** Acceptance-Test Phase 1 am Gerät: flashen
(`idf.py flash monitor`), prüfen dass UI gerendert wird, Scrolling
funktioniert und Mic-Button-Touch den Log `Mic button pressed` auslöst.
Dabei Touch-Kalibrierung verifizieren: in `main/drivers/touch.c` sind
typische Default-Rohwertspannen hardcodiert (`TOUCH_RAW_*`,
`TOUCH_SWAP_XY`, `TOUCH_INVERT_*`); bei jedem neuen Druck wird
`raw=(x,y) -> px=(x,y)` geloggt, damit lassen sich die 4 Eckpunkte
ausmessen und die Defines anpassen. Danach Phase 2 (WiFi + esp-sr +
Wake Word).

**Offene Fragen/Blocker:** Touch-Kalibrierwerte und Farbdarstellung
(Byte-Swap) sind ohne Hardware ungetestet — Verifikation am Gerät nötig.

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
