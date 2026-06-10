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
| 1 | Projekt-Skelett + LVGL + UI-Mockup | ⬜ offen | |
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

**Stand 2026-06-10:** Alle Hardware-Komponententests erfolgreich
abgeschlossen und committed (Mic, Amp, Display, Touch – siehe
COMPONENT_TESTS.md). `main/display_test.c` und `main/touch_test.c`
dienen als Referenzimplementierungen für Phase 1.

**Nächster Schritt:** Phase 1 starten – neues ESP-IDF-Projektskelett
mit LVGL + `esp_lcd_ili9341` aufsetzen, `display.c`/`touch.c` aus den
Testdateien ableiten, statisches UI-Layout (Status-Bar, Chat-Container,
Mic-Button) gemäß Spec Section 4 bauen.

**Offene Fragen/Blocker:** keine.

---

## Wichtige Entscheidungen / Abweichungen vom Spec

_(Noch keine Abweichungen. Trage hier Änderungen ein, die während der
Implementierung von der Spec abweichen – z.B. andere GPIOs, andere
Modellnamen, andere Bibliotheksversionen – damit die Spec nicht ständig
angepasst werden muss.)_

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
