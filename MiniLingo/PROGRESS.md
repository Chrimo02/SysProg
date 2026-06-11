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
| 2 | WiFi + esp-sr + Wake Word | ✅ erledigt | 2026-06-10, am Gerät verifiziert: Wake Word 3/3 Detections, WiFi verbindet (2,4-GHz-SSID) |
| 3 | Audio-Capture + Recording-Buffer | ✅ erledigt | 2026-06-10, am Gerät verifiziert: WAV via UART-Dump extrahiert, Sprache anhörbar |
| 4 | MultiNet + Lokale Commands | ✅ erledigt | 2026-06-11, User-Test ok bis auf "thirty minutes" (niedrige Prio, s.u.) |
| 5 | Groq Whisper Integration | ✅ erledigt | 2026-06-12, User-Test bestanden (inkl. Touch-Trigger + Farb-Fix, Transkript-Bubble ok) |
| 6 | Gemini Flash Integration | ✅ erledigt | 2026-06-12, User-Test bestanden (Lautsprecher-Test davor: 2 Runden inkl. History-Follow-up) |
| 7 | Gemini TTS + Wiedergabe | 🟡 implementiert | 2026-06-12; E2E-Test am Gerät blockiert: TTS-Tageslimit (10 Req./Tag) erschöpft — Hörtest + User-Test stehen aus, s.u. |
| 8 | Polish + Robustness | ⬜ offen | |
| 9 (optional) | AEC | ⬜ offen | |
| 10 | Function-Calling für freie Timer-Werte | ⬜ offen | **Nicht mehr optional** — User-Anforderung 2026-06-11: jede gewünschte Sekunden-/Minutenzahl muss über Gemini-Function-Calls funktionieren (MultiNet kann nur das feste 6er-Set) |

---

## Aktueller Stand / Nächster Schritt

**Stand 2026-06-12 (Session 9): Phase 7 implementiert und geflasht;
der finale Hörtest ist durch das Gemini-TTS-Tageslimit blockiert.**
Die Free-Tier-Quota für `gemini-2.5-flash-tts` ist **10 Requests/Tag**
(per curl verifiziert: 429, `GenerateRequestsPerDayPerProjectPerModel-
FreeTier, quotaValue 10` — deutlich weniger als die angenommenen ~15)
und war nach 2 Test-Calls erschöpft. Aufbau:

- `main/network/tts.c`: TTS-Request nach Spec Section 9.5
  (`responseModalities=AUDIO`, Voice aus `GEMINI_VOICE_NAME`),
  Response-JSON (bis 1,5 MB) + dekodiertes PCM (bis 1 MB) in
  statischen PSRAM-Buffern, `inlineData.data` per
  `mbedtls_base64_decode`, Sample-Rate aus dem mimeType
  (`audio/L16;codec=pcm;rate=24000`). HTTP 429 →
  `ESP_ERR_NOT_ALLOWED` → UI-Bubble "Cloud: TTS daily limit"
  (sonst "Speech failed").
- `main/local/canned_audio.c`: Queue-Items sind jetzt ein struct
  (SPIFFS-Name ODER roher PCM-Buffer); `canned_audio_play_pcm()`
  blockiert per Done-Semaphore bis zum Wiedergabe-Ende. Dadurch
  serialisieren sich Canned- und TTS-Wiedergabe im selben Player-Task
  und `canned_audio_stop()` ("stop") bricht beides ab.
- `main/network/cloud.c`: nach `CLOUD_EVT_REPLY` folgt
  `speak_reply()` — TTS-Request, dann blockierende Wiedergabe; busy
  bleibt bis zum Wiedergabe-Ende gesetzt (= kein neues Wake Word,
  kein Self-Trigger). Neue Events `CLOUD_EVT_SPEAKING`/`CLOUD_EVT_DONE`,
  neuer Getter `cloud_is_speaking()`.
- `main/speech/afe_pipeline.c`: während SPEAKING läuft eine
  MultiNet-Session mit (Neustart bei Match/6-s-Timeout, Ende mit der
  Wiedergabe). `main.c` ignoriert dabei alle Matches außer `CMD_STOP`
  (False-Positive-Schutz gegen das eigene Lautsprecher-Signal);
  "stop" während SPEAKING stoppt nur die Wiedergabe, NICHT den
  Küchen-Timer. UI: Mic-Button grün + "Speaking..." (SPEAKING), nach
  DONE/ERROR zurück zu Idle.

**Verifiziert am Gerät (Lautsprecher-Test):** Pipeline bis zum
TTS-Call komplett — "Wie viele Planeten hat unser Sonnensystem?" →
Whisper wortgenau → Gemini "Unser Sonnensystem hat acht Planeten."
→ TTS-Request korrekt aufgebaut, 429 sauber abgefangen (Error-Bubble,
zurück zu Idle, nächste Session startet normal). Der PCM-Wiedergabe-
Pfad (Queue-Item, 24-kHz-Umschaltung, Done-Semaphore) ist per
temporärem Sinus-Selbsttest am Gerät verifiziert (1 s Ton, exakt
1000 ms, ESP_OK; Testcode wieder entfernt). Der TTS-Request selbst
war vor dem Quota-Aus per curl verifiziert (200, Audio dekodierbar,
Format exakt wie Spec 9.5). **Nicht getestet:** echtes TTS-Audio
end-to-end am Gerät + "stop" während der Wiedergabe.

**Nächster Schritt:** Sobald die Quota zurückgesetzt ist (täglich,
Mitternacht Pacific ≈ 9 Uhr deutscher Zeit): Hörtest — Wake Word →
Frage → gesprochene Antwort; "stop" während der Wiedergabe; danach
User-Test. Achtung beim Testen: nur 10 TTS-Requests/Tag, jede freie
Frage verbraucht einen. Das TODO "Canned-Voice-Samples männlich
regenerieren" (unten) nutzt dieselbe Quota (gleiches TTS-Modell) —
Stimmen-Proben und Phase-7-Tests besser nicht am selben Tag.

---

**Stand 2026-06-12 (Session 8): Phase 6 implementiert, geflasht und
per Lautsprecher-Test verifiziert.** Whisper-Transkripte gehen jetzt
an Gemini 2.5 Flash, die Antwort erscheint als Assistant-Bubble.
Testdialog (2 Runden): "Wie heißt die Hauptstadt von Australien?" →
"Die Hauptstadt von Australien ist Canberra." (1,3 s); Follow-up
"Und wie viele Einwohner hat diese Stadt?" → "Canberra hat etwa
460.000 Einwohner." (0,8 s) — **der History-Ringpuffer löst den
Kontextbezug korrekt auf**. Aufbau:

- `main/network/gemini.c`: JSON-Request per cJSON (systemInstruction
  aus Spec Section 9.4, contents = History + neuer User-Turn,
  maxOutputTokens 200, temperature 0.7), Antwort aus
  `candidates[0].content.parts[].text`, trailing Whitespace getrimmt.
- `main/state.c`: History-Ringpuffer (10 Turns à 1 KB, PSRAM),
  Zugriff nur im network_task. Gepusht wird erst NACH erfolgreichem
  Gemini-Call (User+Model als Paar) — bei Fehlern bleibt die History
  konsistent.
- `main/network/cloud.c`: network_task hängt Gemini sequenziell an
  Whisper an (TLS nie parallel); neues `CLOUD_EVT_REPLY`. `main.c`:
  PROCESSING bleibt bis zur Gemini-Antwort stehen, dann
  Assistant-Bubble + Mic-Button idle.
- `main/network/http_util.c`: `write_all`/`read_body` aus whisper.c
  herausgezogen, nutzen jetzt beide Clients.
- **`thinkingConfig.thinkingBudget = 0`** im Request: gemini-2.5-flash
  "denkt" per Default — das kostet Latenz und würde das
  200-Token-Budget auffressen (leere Antwort mit
  finishReason=MAX_TOKENS). Mit Budget 0 antwortet Flash in 0,6–1,3 s.

**Beobachtung aus dem Test (Phase-8-Kandidat):** Bei einem
Fehlversuch (Frage kam erst nach dem 1,5-s-Stille-Cutoff) halluzinierte
Whisper aus der fast leeren Aufnahme **"Vielen Dank."** (klassische
Whisper-Stille-Halluzination), Gemini antwortete artig "Gern
geschehen!". Falls das im Alltag auftritt: Aufnahmen, die der Recorder
als (nahezu) reine Stille beendet, gar nicht erst hochladen.
Außerdem erneut ein MultiNet-False-Positive ("stop", prob 0.09, fraß
einen Query) — bekanntes Problem, siehe Session 7. Und: echte
Gemini-Antworten enthalten Umlaute — der ASCII-Font (Montserrat 14)
zeigt die nicht an; der Custom-Font (Phase 8) wird damit drängender.

**User-Test 2026-06-12 bestanden.** Phase 6 damit komplett.

**Nächster Schritt:** Phase 7 — Gemini TTS + Wiedergabe (`tts.c`:
`responseModalities=AUDIO`, Response in PSRAM, Base64-Decode → PCM
24 kHz über `audio_out`; WakeNet während SPEAKING aus; "stop" bricht
ab). Einstiegspunkt wieder `network_task` in `main/network/cloud.c`.

---

**Stand 2026-06-12 (Session 7): Phase 5 implementiert, geflasht und
per Lautsprecher-Test verifiziert.** Freie Fragen landen nach dem
MultiNet-Timeout bei Groq Whisper; beide Testsätze ("Wie wird das
Wetter morgen in Berlin?", "Erzähl mir einen kurzen Witz über
Roboter.") kamen wortgenau zurück (~1,5 s für 6 s Audio). Aufbau:

- `main/network/whisper.c`: Multipart-Upload des WAV-Buffers an
  `api.groq.com` (`whisper-large-v3-turbo`, `language` aus neuem
  Kconfig `WHISPER_LANGUAGE`, Default `de`), Response per cJSON.
- `main/network/cloud.c`: `network_task` (Prio 3, Core 0, 16 KB,
  Spec Section 8) mit Job-Queue; Phase 6/7 (Gemini, TTS) haengen
  sich hier an. TLS-Requests laufen strikt sequenziell.
- Cloud-Trigger: `commands.c` meldet den MultiNet-Timeout per
  Callback; `main.c` startet den Upload erst, wenn Capture-Ende UND
  Timeout beide da sind (beides im afe_fetch_task, kein Locking).
  Ein MultiNet-Match nach Capture-Ende verwirft die Aufnahme.
- Buffer-Schutz: solange `cloud_is_busy()`, nimmt `afe_pipeline.c`
  kein neues Wake Word an — `recorder_start()` wuerde sonst den
  WAV-Buffer ueberschreiben, den der Upload gerade liest.
- UI: Transkript als User-Bubble, Mic-Button orange + "Processing..."
  waehrend der Cloud-Pipeline (die "Captured ..."-Bubble ist ersetzt).
- **mbedTLS allokiert jetzt aus dem PSRAM**
  (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, sdkconfig + defaults): der
  Handshake (~50 KB) scheiterte neben esp-sr mit
  `mbedtls_ssl_setup -0x7F00` (alloc failed) im internen Heap.

**Bekanntes Problem (2026-06-12): MultiNet-False-Positives bei
deutschen/freien Saetzen.** Im Test matchte "Wie wird das Wetter..."
einmal als "mute" (prob 0.05) und verwarf den Cloud-Query; auch auf
Umgebungsgeraeusche gab es Matches ("mute" 0.17, "unmute" 0.20 nach
spontanen Wake-Detections). Threshold 0.05 ist fuer die Timer-Phrasen
noetig (siehe Session 6) — falls das im Alltag nervt: Threshold nur
fuer die kurzen Ein-Wort-Commands anheben (per-Command-Threshold
pruefen) oder Mindest-Dauer checken; Phase 8 (Robustness).

**Nachtrag 2026-06-12 (gleiche Session): Farb-Fix + Touch-Trigger.**
User-Beobachtung: User-Bubbles orange statt blau, Listening-Button
blau statt rot — das Panel ist ein BGR-ILI9341, `display.c` stand
auf `ESP_LCD_COLOR_SPACE_RGB` (fiel nie auf: Grau/Gruen ueberleben
den R/B-Tausch fast unveraendert). Fix: `..._BGR`. Ausserdem den
Mic-Button als Touch-Trigger aktiviert (stand in der Spec als
Trigger "Wake Word | Touch", war aber keiner Phase zugeordnet):
`afe_pipeline_request_listening()` setzt ein atomares Flag, der
afe_fetch_task startet damit dieselbe Session wie nach dem Wake Word
(gleicher Listener-Pfad, MultiNet + Cloud-Fallback inklusive).
Ignoriert, solange Session oder Cloud-Pipeline laufen. Beobachtung
aus dem Regressionstest: MultiNet verfehlte einmal "set timer five
minutes" (bekannt: niedrige Probs) — der Cloud-Fallback fing es ab;
Whisper transkribierte "Set Set Timer 5 Minutes": die
vad_cache-Frame-Doppelung aus Session 6 ist also auch im WAV
hoerbar (fuer Whisper unkritisch, ggf. Phase 8 pruefen, ob der
Recorder den vad_cache wirklich braucht).

**User-Test 2026-06-12 bestanden:** Touch-Trigger funktioniert,
Farben stimmen jetzt (User-Bubble blau, Listening rot), Transkript
erscheint als Bubble. Phase 5 damit komplett.

*(Nächster Schritt von damals — Phase 6, Gemini Flash — ist erledigt,
siehe Session 8 oben.)*

---

**Stand 2026-06-10 (Session 5):** Phase 4 implementiert, gebaut und
geflasht (inkl. SPIFFS-Image). Boot-Log sauber: `mn7_en ready: 14
commands, chunk 512 samples`, `SPIFFS mounted, 0 canned file(s)
missing`, AFE-Pipeline läuft. Aufbau:

- `main/speech/commands.c`: MultiNet7-EN aus der model-Partition,
  14 Commands (Spec Section 7), Timeout 6s. Der `afe_fetch_task`
  feedet MultiNet parallel zum Recorder (gleiche Frames inkl.
  `vad_cache`); ein Sammel-Buffer puffert auf die MultiNet-
  Chunk-Größe (derzeit identisch zur AFE-Chunk-Größe, 512 Samples).
  Match → `on_local_command()` in `main.c` (verwirft die Aufnahme per
  `recorder_abort()`, führt den Handler aus, spielt die
  Canned-Response); MultiNet-Timeout → Aufnahme läuft weiter und ist
  ab Phase 5 der Cloud-Pfad.
- `main/local/timer.c`: Kitchen-Timer als One-Shot-`esp_timer`,
  Restzeit aus gespeicherter Deadline (atomar) → Status-Bar zeigt
  `Timer: MM:SS` (LVGL-Timer in `main.c`). Bei Ablauf: Canned
  "Your timer is done." + Alarm-Beeps.
- `main/local/volume.c` + `main/drivers/audio_out.c`: I2S1/MAX98357A
  (Pins aus `amp_test.c`), Lautstärke als Q15-Software-Gain über die
  Samples, Stufen 0–100% in 20er-Schritten, mute merkt sich den
  vorherigen Wert. Rate pro File umschaltbar (16k canned / 24k
  Gemini-TTS ab Phase 7).
- `main/local/canned_audio.c`: SPIFFS-Partition `storage` gemountet,
  Player-Task (= `audio_out_task` der Spec, Prio 5/Core 1) streamt
  WAVs chunk-weise; Queue Tiefe 4, `canned_audio_stop()` für den
  "stop"-Command. WAV-Parser läuft chunk-weise (RIFF), weil afconvert
  einen FLLR-Chunk vor `data` einfügt.
- `scripts/generate_canned_tts.py`: erzeugt die 13 Phrasen + Alarm
  nach `spiffs/` (~630 KB, 16 kHz mono). Engine `gemini` (Spec) oder
  `say` (macOS, offline) — mangels `GEMINI_API_KEY` aktuell mit `say`
  generiert; mit Key einfach neu laufen lassen. SPIFFS-Image wird via
  `spiffs_create_partition_image` bei `idf.py flash` mitgeflasht.
- `main.c`: Command-Dispatch + UI-Events (grüne Local-Bubble pro
  Command, Mic-Button kurz grün, Volume-Anzeige, Timer-Countdown).

**Hardware-Vorfall (2026-06-11, behoben):** Nach dem Phase-4-Flash
reagierte das Wake Word nicht mehr — das Mic lieferte konstant
Vollausschlag. Ursache war ein Kurzschluss im Aufbau zwischen
Amp-BCLK (GPIO 15) und Mic-SD (GPIO 6), der erst auffiel, seit
Phase 4 den I2S1 aktiv taktet (vorher: GPIO 15 nie angesteuert, alle
Einzeltests grün). Diagnose: Mic-RMS-Log (`afe_pipeline.c`, Debug-
Level) zeigte rms 32767; mic_test solo ok; mic+amp-Kombi-Test las
0x555555 (= 512-kHz-BCLK); GPIO-Toggle-Test bewies GPIO15→GPIO6.
Verkabelung korrigiert, Wake Word wieder verifiziert (Ruhe-RMS
15–20, Detections ok). Der Kurzschluss erklärt vermutlich auch den
zwischenzeitlichen Brownout/USB-Ausfall (Bus-Kontention Mic gegen
GPIO-15-Treiber). Merksatz: Bei "Mic liefert konstant Max" zuerst
Pegel-Log auf INFO heben und Leitungen per GPIO-Toggle durchpruefen.

**MultiNet-Debugging (2026-06-11, Session 6):** Der erste Sprachtest
zeigte drei Probleme; alle behoben (Tests per `say` über die
Mac-Lautsprecher + Serial-Log, jeweils am Gerät verifiziert):

1. *"set timer X minutes" matchte nie.* Mehrstufige Diagnose
   (Decoder-Rohtext + registrierte Phoneme geloggt): Das G2P-Token
   `MgNcTS` ("minutes") ist im FST von mn7_en (esp-sr 2.4.6)
   **defekt** — jede Phrase damit bleibt ewig im Detecting-State,
   unabhängig von Threshold/Aliassen. Workaround in `commands.c`:
   Plural-Phrasen als Phonem-Registrierung ohne Schluss-S
   (`SfT TiMk FiV MgNcT`), das gesprochene S wird als Junk-Suffix
   toleriert. Zusätzlich Kurz-Aliasse ("five minute timer") mit
   akustisch korrektem `MgNcT` ("min-it" — die On-Device-G2P erzeugt
   fälschlich `MgNoT` = "min-oot"). Verifiziert: five 3/3 (0.09–0.11),
   ten/fifteen/one/thirty-seconds ✓.
2. *MultiNet-Feintuning:* `vad_cache` geht nicht mehr an MultiNet
   (überlappte mit schon gelieferten Frames → Decoder hörte doppelte
   Phrasenanfänge "SfPSfT"); die MultiNet-Session läuft jetzt bis zum
   eigenen Match/Timeout weiter statt mit dem Recorder-Ende zu sterben
   (MultiNets Endpoint braucht bei langen Phrasen länger als 1,5s
   Stille); Detection-Threshold explizit 0.05 (Timer-Phrasen matchen
   systembedingt niedrig, 0.05–0.15; freie Sätze lösen damit weiterhin
   nichts aus — bei 0.01 gab es False-Positives). Achtung:
   `switch_loader_mode()` ist im quantisierten mn7_en ein
   NULL-Pointer (Boot-Loop per InstrFetchProhibited) — nicht aufrufen.
3. *Knattergeräusch nach jeder Wiedergabe:* ohne `auto_clear` loopt
   der I2S-DMA den letzten Buffer endlos (`audio_out.c`: behoben).
   *"stop timer" matcht als "stop":* MultiNet feuert nach "stop",
   bevor "timer" dekodiert ist → `CMD_STOP` cancelt jetzt auch einen
   laufenden Timer (main.c). *WAV-Dump deaktiviert* (sdkconfig) —
   er schaltete nach jedem Capture 30s die Logs stumm; war eh ab
   Phase 5 fällig.

**Acceptance-Test Phase 4 (per Mac-Lautsprecher bestanden, User-Test
ausstehend):** "Mycroft" → "set timer five minutes" → Ansage + Timer
läuft. Verifiziert außerdem: "how much time left", "cancel timer",
"stop", "volume up/down", "mute"/"unmute", freie Sätze → Timeout →
Cloud-Pfad. Vom User zu testen: echte Stimme statt `say`, Countdown
in der Status-Bar, Alarm nach Ablauf, "stop" während des Alarms.

*(Nächster Schritt von damals — User-Sprachtest Phase 4 und Phase 5 —
ist erledigt, siehe Session 7 oben.)* API-Keys (Groq + Gemini) sind
seit 2026-06-11 in menuconfig gesetzt; Canned-WAVs mit Gemini-TTS
(Voice Kore) regeneriert.

**TODO Canned-Voice-Samples (User-Wunsch 2026-06-11):** Die
Voice-Samples nochmal neu generieren mit einer **männlichen Stimme**
— Wunschrichtung: „britischer Gentleman". Der User will die
Stimmen-Auswahl **selbst anhören und entscheiden**. Vorgehen:
1. Pro Kandidat-Stimme 1 Probe-Satz generieren (männliche
   Gemini-Voices: Puck, Charon, Fenrir; Stil lässt sich zusätzlich
   per Text-Prompt steuern, z.B. "Say like a refined British
   gentleman: ..." — ausprobieren, was die TTS-Preview hergibt)
   und dem User lokal vorspielen (`afplay`).
2. Gewählte Stimme: alle 13 Phrasen via
   `scripts/generate_canned_tts.py --engine gemini --voice <X>`
   regenerieren (Achtung Free-Tier: ~15 Requests/Tag — Proben und
   Vollgenerierung ggf. auf zwei Tage verteilen; `--skip-existing`
   zum Fortsetzen) und `GEMINI_VOICE_NAME` in menuconfig angleichen,
   damit die Phase-7-Antworten dieselbe Stimme bekommen.
   Nebenbei: 4 Files sind noch `say`-Stimme (tts_time_left,
   tts_volume_changed, tts_muted, tts_unmuted) — Quota war
   erschöpft; werden dabei mit ersetzt.

**Offene Fragen/Blocker:** keine. Timer-Phrasen matchen mit niedrigen
Wahrscheinlichkeiten (0.05–0.15) — falls beim User-Test einzelne
Phrasen nicht greifen, zuerst Decoder-Rohtext loggen
(`get_results()->raw_string` im Detecting-State) und Phonem-Alias
ergänzen. User-Test 2026-06-11: alle Timer-Werte ok außer
**"thirty minutes"** (matcht nicht; vermutlich Kollision mit
"thirty seconds", beide beginnen `vkDm`) — niedrige Prio, weil
Phase 10 (Function-Calling, jetzt fest eingeplant) ohnehin beliebige
Timer-Werte über die Cloud nachliefert. Touch-Feinkalibrierung weiterhin optional. Hinweise aus
Phase 2, die weiter gelten: WakeNet-Threshold auf Model-Default
belassen (steht im Model-Info-String des Boot-Logs); ESP32-S3 kann
nur 2,4-GHz-WiFi (5-GHz-SSID → reason 201).

**Wake Word seit 2026-06-11: "Mycroft"** (`wn9_mycroft_tts`,
Model-Default-Threshold 0.625) statt "Alexa" — User-Wunsch. Wechsel =
nur menuconfig (Select wake words) + `idf.py flash` (Model-Partition
wird neu gepackt) + Idle-Text in `main.c`; die AFE nimmt das Modell
automatisch aus der Partition. Hinweis: _tts-Modelle sind auf
synthetischen Stimmen trainiert und ggf. etwas unzuverlaessiger als
wn9_alexa/wn9_hiesp — falls die Erkennung im Alltag nervt, Threshold
per CONFIG_WAKE_WORD_THRESHOLD senken oder Modell zuruecktauschen.
Verifiziert per Lautsprecher-Test: 4/4 Detections + Timer-Command
end-to-end.

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
- **"how much time left" (Command 9) sagt keine variable Restzeit an**:
  Canned-Phrasen sind fix, deshalb spielt der Command "The remaining
  time is on the display." und die exakte Zeit steht in Bubble +
  Status-Bar. Echte Ansage wäre erst mit Cloud-TTS (Phase 7) möglich.
- **Canned-Files erweitert ggü. Spec-Liste** (Section 7):
  zusätzlich `tts_timer_set_30s`, `tts_no_timer` (stop/cancel/time-left
  ohne laufenden Timer), `tts_time_left`, `tts_unmuted`, `tts_alarm`
  (synthetische Beeps, hängt nach `tts_timer_done` dran). Die
  Dateinamen-Konstanten leben in `main/local/canned_audio.h`, die
  Phrasen in `scripts/generate_canned_tts.py`.
- **Model-Liste wird einmal in `main.c` geladen** (`esp_srmodel_init`)
  und an `commands_init()` (MultiNet) und `afe_pipeline_init(models)`
  (WakeNet/AFE) gereicht — Commands müssen vor dem Pipeline-Start
  initialisiert sein, weil der fetch-Task sie ab dem ersten Wake Word
  füttert.
- **Timer-Commands brauchen Phonem-Registrierung** (mn7_en-Bugs:
  FST-Token `MgNcTS` defekt, G2P spricht "minute" als "min-oot") —
  Details im Kommentarkopf von `main/speech/commands.c` und unter
  "MultiNet-Debugging" oben. Zusätzlich zu den Spec-Phrasen gibt es
  deshalb "X minute timer"-Aliasse; `esp_mn_commands_get_string()`
  liefert fürs UI die Grapheme-Phrase des Matches.
- **"stop" cancelt auch den laufenden Timer**: MultiNet matcht
  "stop timer" oft schon nach "stop" (Endpoint vor "timer") — der
  CMD_STOP-Handler in `main.c` deckt beide Intents ab.
- **mbedTLS allokiert aus dem PSRAM** (Phase 5,
  `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`): TLS-Handshake (~50 KB) passt
  neben esp-sr nicht in den internen Heap. Spec Section 13 ("TLS
  frisst RAM") gilt weiterhin: nie zwei TLS-Verbindungen parallel —
  deshalb laufen alle Cloud-Requests sequenziell im `network_task`.
- **Kein neues Wake Word waehrend die Cloud-Pipeline laeuft**
  (`cloud_is_busy()`-Gate im afe_fetch_task): der Whisper-Upload
  liest direkt aus dem Recording-Buffer; `recorder_start()` wuerde
  ihn ueberschreiben. Entspricht dem PROCESSING-State der
  Spec-State-Machine (Section 8).
- **`WHISPER_LANGUAGE` als Kconfig** (Default `de`, Spec Section 9.3):
  die lokalen Commands sind englisch, freie Fragen deutsch — falls
  sich das aendert, nur menuconfig statt Code.
- **Das ILI9341-Panel ist BGR** (`ESP_LCD_COLOR_SPACE_BGR` in
  `display.c`, 2026-06-12): mit RGB waren Rot und Blau vertauscht.
  Unabhaengig vom Endianness-Byte-Swap im Flush-Callback.
- **`state.c` enthält nur den History-Ringpuffer** (Phase 6): die
  State-Machine aus Spec Section 8 ist weiterhin als atomare Flags in
  `main.c` umgesetzt (Entscheidung aus Phase 5). Kein Locking — alle
  History-Zugriffe laufen im network_task.
- **Gemini-Thinking abgeschaltet** (`thinkingBudget: 0`, Phase 6,
  steht nicht in der Spec): gemini-2.5-flash ist ein Thinking-Model
  mit dynamischem Default — ohne das Abschalten frisst das Denken
  Latenz und das maxOutputTokens-Budget (leere Antwort mit
  finishReason=MAX_TOKENS möglich). Konstante in
  `main/network/gemini.c`.
- **History nur mit vollständigen Paaren**: User- und Model-Turn
  werden erst nach erfolgreichem Gemini-Call gepusht; ein
  fehlgeschlagener Request hinterlässt keinen halben Turn.
- **TTS-Wiedergabe läuft über die Canned-Audio-Queue** (Phase 7,
  statt einer separaten tts_queue wie in Spec Section 9.5): ein
  Player-Task = ein I2S-Schreiber, "stop" (canned_audio_stop) bricht
  Canned UND TTS ab, und Alarm/TTS serialisieren sich automatisch.
  `canned_audio_play_pcm()` blockiert den network_task bis zum
  Wiedergabe-Ende — dadurch bleibt `cloud_is_busy()` während SPEAKING
  gesetzt (kein neues Wake Word = kein Self-Trigger, Spec Section 13).
- **Während SPEAKING akzeptiert MultiNet nur "stop"** (Phase 7): das
  Mikrofon hört den eigenen Lautsprecher, und Threshold 0.05 ist
  False-Positive-anfällig (siehe Session 7) — alle anderen Matches
  werden in `on_local_command()` verworfen. "stop" stoppt dabei nur
  die Wiedergabe, nicht den Küchen-Timer (anders als im IDLE, wo
  "stop" als "stop timer" gilt).
- **Gemini-TTS-Free-Tier: 10 Requests/Tag** (verifiziert 2026-06-12,
  Quota `GenerateRequestsPerDayPerProjectPerModel-FreeTier`), nicht
  ~15 wie zuvor angenommen. HTTP 429 → eigene UI-Meldung
  "TTS daily limit"; Retry/Backoff kommt in Phase 8.
- **Touch-Trigger (Mic-Button) ist implementiert** (2026-06-12; in
  der Spec als Trigger vorgesehen, aber keiner Phase zugeordnet):
  LVGL-Klick -> `afe_pipeline_request_listening()` (atomares Flag)
  -> afe_fetch_task startet die normale Listening-Session ohne
  WakeNet. `wake_word_on_touch()` feuert denselben Listener wie das
  Wake Word.

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
