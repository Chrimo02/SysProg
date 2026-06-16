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
| 7 | Gemini TTS + Wiedergabe | ✅ erledigt | 2026-06-12, E2E vom User bestätigt. **TTS auf User-Wunsch pausiert** (Latenz + Free-Tier-Limit 10 Req./Tag) — Text-Antwort genügt einstweilen, bis eine performantere TTS-Lösung gefunden ist. Details s.u. |
| 8 | Polish + Robustness | ✅ erledigt | 2026-06-12, User-Test bestanden (Umlaute, Puls/Spinner, Error-Beep). **Offen: WiFi-Drop-Test** (TODO unten — User testet, wenn Router-Zugriff da ist) |
| 8b | Volume per Touch-Slider + Sprach-Stufen | ✅ erledigt | 2026-06-12, User-Test bestanden (inkl. Slider-Fix + "sixty"-Aliasse, s. Session-Notiz) |
| 8c | LLM-Wechsel Gemini → Cerebras | ✅ erledigt | 2026-06-12, E2E am Gerät verifiziert (inkl. History-Follow-up); Antwortzeit ~500 ms (Gemini: 600-1300 ms). Modell: gpt-oss-120b |
| 9 (optional) | AEC | ⬜ offen | |
| 10 | Function-Calling für freie Timer-Werte | ✅ erledigt | 2026-06-16, gebaut (Build grün). OpenAI-Tool-Format gegen Cerebras: set_timer/set_volume/cancel_timer/get_time_left. **User-Test am Gerät ausstehend** (s.u.) |

---

## Aktueller Stand / Nächster Schritt

**Nachtrag 2026-06-16 (Session 11, Fortsetzung): TTS-Toggle-Button
(User-Wunsch).** Kleiner Button unten rechts im Mic-Bereich
(`main/ui/ui_tts_button.c`, "TTS"-Label, grün = an / grau = aus)
schaltet das Vorlesen der Antworten zur Laufzeit. Klick →
`on_tts_button_clicked()` in main.c (LVGL-Task) → `cloud_set_tts_enabled()`
(atomar) + Button-Optik + Bubble "Voice replies on/off".

- `CONFIG_TTS_SPEAK_REPLIES` ist jetzt nur noch der **Startwert** des
  Laufzeit-Flags (default n), kein reiner Compile-Schalter mehr.
- Die **2,5 MB PSRAM-TTS-Buffer werden lazy** beim ersten tatsächlichen
  Vorlesen allokiert (`s_tts_inited`-Guard in `speak_reply`), nicht mehr
  in `cloud_init` — aus bleibt also exakt so speichersparsam wie zuvor.
- Achtung Alltag: TTS nutzt Gemini (10 Requests/Tag Free-Tier) und
  bringt 3-5 s Latenz — der Button macht das jetzt bewusst pro Session
  steuerbar, der "stop"-während-Wiedergabe-Test (Session 9) ist beim
  Aktivieren weiterhin offen.
- **Build grün, geflasht. User-Test ausstehend** (Button togglet,
  gesprochene Antwort kommt bei "an", Optik/Bubble stimmen).

---

**Stand 2026-06-16 (Session 11): Phase 10 implementiert und gebaut
(Build grün), User-Test am Gerät ausstehend.** Das LLM (Cerebras
gpt-oss-120b) kann jetzt per OpenAI-Tool-Calling die echten Aktionen
für *beliebige* Werte auslösen, die die lokalen MultiNet-Commands
nicht abdecken. Vier Tools: `set_timer` (Sekunden), `set_volume`
(Prozent), `cancel_timer`, `get_time_left`.

Umsetzung (Single-Call, kein zweiter LLM-Round-Trip — spart Token +
Latenz, bewusste Entscheidung passend zur Cerebras-Quota-Motivation):

- `main/network/llm.h`: `llm_generate()` liefert jetzt ein
  `llm_result_t` (Discriminated Union TEXT | TOOL_CALL) statt eines
  reinen Text-Buffers.
- `main/network/llm.c`: System-Prompt erweitert (Timer/Volume per Tool
  steuern, Minuten → Sekunden umwandeln); `build_request()` hängt ein
  `tools`-Array mit den 4 Schemas an (`tool_choice` Default `auto`);
  `parse_reply()` prüft `choices[0].message.tool_calls` VOR `content`
  (`function.arguments` ist im OpenAI-Format ein JSON-**String**, wird
  in run_tool separat geparst).
- `main/network/cloud.c`: neuer Helper `run_tool()` führt im
  `network_task` die Aktion aus und baut eine deterministische
  Bestätigung im Wortlaut von `format_command_bubble` (main.c) —
  "Timer set: M:SS", "Volume: NN%", "Timer cancelled" / "No timer
  running", "Time left: M:SS". Confirmation geht über den bestehenden
  `CLOUD_EVT_REPLY`-Pfad (graue Assistant-Bubble), History führt den
  Bestätigungstext als Model-Turn → Follow-ups ("zwei Minuten kürzer")
  haben Kontext. **Kein neuer Task/keine Queue:** `kitchen_timer_start`
  nutzt nur `esp_timer` + atomics, `volume_set` ist atomar,
  `canned_audio_play` queued nur — alles aus dem network_task sicher
  (eine frühere Annahme, es brauche eine Dispatch-Queue, war falsch).

**Nicht geändert:** `state.c` (kein CHAT_ROLE_TOOL nötig), keine neuen
Canned-Audio-Files (beliebige Werte → Text-Bubble, TTS ist ohnehin per
Kconfig aus).

**Nachbesserung nach erstem User-Test (2026-06-16): Volume-Anzeige
aktualisierte bei Cloud-`set_volume` nicht** (Slider + Prozentanzeige
blieben stehen). Ursache: die Status-Bar-Lautstärke wurde nur beim
lokalen Command (`s_cmd_event`) bzw. beim Slider-Drag gesetzt — der
Cloud-Tool-Pfad emittiert nur `CLOUD_EVT_REPLY`. Fix in `main.c`: neues
`update_volume_display()` pollt jeden LVGL-Tick `volume_get()` und
schreibt die Anzeige nur bei Änderung (Guard `s_volume_shown`, analog
zu `update_state_text()`/Timer-Countdown). Deckt jetzt alle Pfade ab;
die explizite Setzung im Command-Zweig ist dadurch entfallen. Geflasht.

**Nächster Schritt: User-Test am Gerät** (`idf.py flash monitor`):
1. "Mycroft" → "Stell einen Timer auf sieben Minuten" → Bubble
   "Timer set: 7:00", Countdown läuft, Alarm nach Ablauf.
2. "Lautstärke auf 35 Prozent" → Log `volume 35%`, Bubble "Volume: 35%".
3. "Wie lange läuft der Timer noch?" → "Time left: M:SS".
4. "Brich den Timer ab" → "Timer cancelled".
5. Regression: "Wie heißt die Hauptstadt von Australien?" → normale
   Text-Antwort (kein Tool-Call).
6. Regression lokal: "set timer five minutes" matcht weiter über
   MultiNet (ohne Cloud).
7. Follow-up: nach gesetztem Timer "Mach ihn zwei Minuten kürzer" →
   set_timer mit korrigiertem Wert aus der History.

Danach ist nur noch Phase 9 (AEC) offen — optional.

---

**Nachtrag 2026-06-12 (Session 10, Fortsetzung 2): Phase 8c —
LLM-Wechsel Gemini → Cerebras (User stieß an Gemini-Tageslimits,
jetzt auch bei Text-Antworten).** Limit-Vergleich (eine
MiniLingo-Frage ≈ 400-800 Token inkl. System-Prompt, History,
Antwort):

- Gemini 2.5 Flash Free: 250 Requests/Tag, 10/min
- Groq Free (Key vorhanden): 100k-200k Token/Tag ≈ 150-500 Fragen —
  kaum besser als Gemini, deshalb verworfen
- **Cerebras Free: 1 Mio. Token/Tag ≈ 1.500-2.500 Fragen, 30/min**
  ← gewählt (User legt Konto an; ~8-10x großzügiger)

Umsetzung: `main/network/llm.c` ersetzt `gemini.c` — OpenAI-
kompatible Chat-Completions (`api.cerebras.ai/v1/chat/completions`,
`Authorization: Bearer`), System-Prompt/History unverändert
übernommen, Retry-Wrapper nach Phase-8-Muster. Neue Kconfigs:
`CEREBRAS_API_KEY`, `LLM_MODEL`. `GEMINI_API_KEY` bleibt für die
(pausierte) TTS. Cerebras kann Tool-Calling — **Phase 10 wechselt
damit aufs OpenAI-Tool-Format** (statt Gemini-Function-Calling wie
in Spec Section 12 angedacht).

**Befund beim Key-Test:** Das geplante Modell `llama-3.3-70b` gibt
es im Cerebras-Free-Tier nicht mehr — `/v1/models` liefert (Stand
2026-06-12) nur **gpt-oss-120b** und **zai-glm-4.7**. Default ist
jetzt `gpt-oss-120b` mit `reasoning_effort=low` (gemessen: ~11
Reasoning-Tokens, 24 ms Serverzeit) und `max_completion_tokens` 300
statt 200, weil Reasoning-Tokens gegen das Budget zählen (gleiche
Falle wie Gemini-Thinking in Phase 6). zai-glm-4.7 ignoriert
reasoning_effort weitgehend (Hinweis in der Kconfig-Help).

**E2E am Gerät verifiziert (2026-06-12):** "Wie heißt die Hauptstadt
von Australien?" → "Die Hauptstadt von Australien heißt Canberra."
(506 ms); Follow-up "Und wie viele Einwohner hat diese Stadt?" →
"Canberra hat etwa 460 000 Einwohner (Stand 2023)." (498 ms) — der
History-Kontextbezug löst korrekt auf, Antwortzeit ~500 ms statt
600-1300 ms bei Gemini. Fehlerpfad (ungültiger Key → 401 →
"Assistant failed") nicht separat geflasht-getestet — derselbe
CLOUD_EVT_ERROR-Pfad ist seit Phase 8 mehrfach verifiziert (No WiFi,
TTS-Limit, Stille-Guard).

**Nächster Schritt:** Phase 10 (Function-Calling, jetzt im
OpenAI-Tool-Format gegen Cerebras). Phase 9 (AEC) optional.

---

**Nachtrag 2026-06-12 (Session 10, Fortsetzung): Phase 8b —
Lautstärke per Touch-Slider und direkte Sprach-Stufen (User-Wunsch).**

- **Touch-Slider**: Klick auf die Volume-Anzeige in der Status-Bar
  öffnet einen Slider (0-100 %) unter der Bar; Auto-Hide nach 3 s
  ohne Interaktion, erneuter Klick schließt sofort. Callback-Pfad wie
  beim Mic-Button (`ui_status_bar_set_volume_changed_cb` → `main.c`
  → `volume_set`, alles im LVGL-Task; der Gain ist atomar).
- **Sprach-Stufen**: 5 neue MultiNet-Commands `CMD_VOLUME_20..100`
  ("volume twenty/forty/sixty/eighty/one hundred percent") — die
  Lautstärke hat nur die 20-%-Stufen von `volume.c`, deshalb reicht
  das feste Phrasenset (kein Cloud-Function-Call nötig; beliebige
  Werte kommen mit Phase 10 gratis dazu).

**Zwei mn7_en-Befunde dabei** (gleiche Werkzeugkiste wie Session 6,
Host-G2P `managed_components/espressif__esp-sr/tool/multinet_g2p.py`
— braucht venv mit g2p_en+pandas plus NLTK-Paket
`averaged_perceptron_tagger_eng`):

1. *"sixty" matchte mit On-Device-G2P nie* → explizite Phoneme für
   alle 5 Phrasen (`VnLYoM ... PkSfNT`); "sixty" zusätzlich als
   gesplittete Tokens `SgKS Tm` statt `SgKSTm` (ein Token bleibt tot,
   wie `MgNcTS` in Session 6).
2. *Globaler Threshold 0.05 → 0.03*: "sixty" matchte nur ~50 % der
   `say`-Versuche (Probs 0.06-0.12, Rest knapp unter 0.05 — die
   "twenty"-Hypothese konkurriert im Decoder, per Decoder-Rohtext-Log
   verifiziert). Bei 0.03 abgesichert durch die per-Command-Mindest-
   Probs: Regressionstest "Wie wird das Wetter morgen in Berlin?"
   feuerte einmal "volume down" mit 0.04 → von min_prob (0.25)
   verworfen, Satz ging normal in die Cloud.

**Verifiziert am Gerät (say-Test):** alle 5 Stufen matchen und setzen
die Lautstärke (twenty 0.06-0.13, forty 0.17, sixty 0.06-0.12,
eighty 0.26, one hundred 0.24); Regression "volume up" 0.92 ok;
freier deutscher Satz → kein False-Positive → Whisper wortgenau.

**Nachbesserung nach User-Test (gleiche Session):** Der erste
User-Test ergab drei Probleme, alle behoben:

1. *Slider von Chat-Bubbles verdeckt + nicht bedienbar*: das Panel
   wird vor dem Chat-Container erstellt und lag in der Z-Reihenfolge
   unter ihm — der Chat verdeckte es UND fing die Touch-Events ab.
   Fix: `lv_obj_move_foreground()` beim Öffnen; zusätzlich
   Ext-Click-Area 14 px (8-px-Track ist für resistiven Touch zu
   schmal). **Slider-Bedienung vom User erneut zu testen.**
2. *"sixty" matchte mit echter Stimme nie* (say-Stimmen ok):
   Aussprache-Aliasse ergänzt — stimmhaftes S wie im Deutschen
   ("zixty" `ZgKS`), offenes/langes i ("sexty" `SfKS` /
   "seexty" `SmKS`). Damit matchen alle 4 Test-Stimmen (Anna/
   Samantha/Daniel/Default) mit 0.13-0.34 statt vorher ~50 %
   Ausfällen; twenty/eighty-Gegenprobe ohne Kreuz-Match.
   **Mit echter Stimme erneut zu testen.**

---

**Stand 2026-06-12 (Session 10): Phase 8 implementiert, geflasht und
am Gerät getestet (per `say`-Lautsprecher-Test).** Umfang:

- **Latin-1-Font** (`main/ui/font_montserrat_14_latin1.c`, generiert
  per `npx lv_font_conv` aus dem LVGL-eigenen Montserrat-TTF;
  Kommandozeile steht im Dateikopf): Umlaute/ß plus typografische
  Anführungszeichen/Gedankenstrich/Ellipse, die Gemini gern ausgibt.
  Nur für die Chat-Bubble-Labels — Status-Bar und Mic-Button brauchen
  die FontAwesome-Symbole des Default-Fonts (Montserrat 14, ASCII).
- **Retry mit Exponential Backoff** (`http_util.h`:
  `HTTP_MAX_ATTEMPTS` 3, Delays 1 s/2 s): whisper/gemini/tts haben
  jetzt `*_once()` + Retry-Wrapper. Transient (wird wiederholt) =
  Verbindungsfehler, HTTP 429, 5xx, kaputte Antworten
  (finishReason=OTHER ohne Audio); permanent = restliche 4xx
  (`ESP_ERR_INVALID_RESPONSE`), fehlender Key, TTS-Tageslimit
  (`ESP_ERR_NOT_ALLOWED`, Warten hilft erst morgen). Der
  Einmal-Retry aus Session 9 in `cloud.c` ist dadurch ersetzt.
- **Kconfig `TTS_SPEAK_REPLIES`** (Default **n**, User-Wunsch aus
  Session 9): TTS komplett abschaltbar; aus spart auch die 2,5 MB
  PSRAM-TTS-Buffer (tts_init wird nicht gerufen). Keine
  "TTS daily limit"-Bubbles mehr im Text-only-Betrieb.
- **Error-Recovery + Audio-Cue**: jeder Cloud-Fehler spielt jetzt
  `tts_error.wav` (kurzer absteigender Doppelton, synthetisch wie der
  Alarm — `gen_error()` in `scripts/generate_canned_tts.py`, kostet
  keine TTS-Quota) und geht zurück zu Idle.
- **Stille-Guard**: Captures mit < 1000 ms VAD-Sprache werden
  verworfen statt hochgeladen (Bubble "Didn't catch that" + Error-Cue).
  Am Gerät vermessen: leere Aufnahmen melden 576-608 ms "Speech"
  (VAD-Nachhall des Wake Words!), eine echte kurze Frage 3712 ms —
  die erste Schwellen-Idee (250 ms) hätte NICHT gereicht, die
  Whisper-Halluzination ("Vielen Dank." → "Gern geschehen!") wurde
  damit am Gerät reproduziert. Mit 1000 ms verifiziert: Discard-Log,
  Error-Beep, kein Upload.
- **Mic-Button-Animationen** (`ui_mic_button.c`): Puls
  (Scale-Animation 100→115 %, 500 ms Playback) bei LISTENING,
  oranger Spinner-Ring um den Button bei PROCESSING.
- **vad_cache geht auch nicht mehr an den Recorder**
  (`afe_pipeline.c`): er überlappt mit bereits gelieferten Frames
  (Session-6/7-Diagnose, Whisper hörte "Set Set Timer") — das Capture
  beginnt ohnehin direkt nach dem Wake Word, der Sprachanfang ist
  auch ohne ihn vollständig.
- **WiFi-Reconnect** (Spec-Phase-8-Punkt) existiert seit Phase 2
  (`wifi.c`: Reconnect-Timer 5 s); **Touch-Kalibrierungs-Screen**
  bewusst weggelassen (laut Komponententest/Alltag nicht nötig,
  "Feinkalibrierung optional").

**Verifiziert am Gerät:** Boot sauber (SPIFFS 15 Files, 0 missing),
WiFi verbindet, "Mycroft" + Stille → Discard + Error-Beep, "Mycroft" +
"Wie heißt die Hauptstadt von Österreich?" → Whisper wortgenau (inkl.
ß/Ö) → Gemini "Die Hauptstadt von Österreich ist Wien." **Vom User zu
testen:** Umlaut-Rendering auf dem Display, Puls/Spinner-Optik,
Error-Beep-Lautstärke, Spec-Acceptance (20 Interaktionen ohne Reset,
WiFi-Drop). Achtung: TTS ist jetzt per Kconfig aus — zum Reaktivieren
`TTS_SPEAK_REPLIES` in menuconfig setzen (der "stop"-während-
Wiedergabe-Test aus Session 9 ist weiterhin offen).

**User-Test 2026-06-12 bestanden:** Umlaut-Rendering, Puls/Spinner-
Optik und Error-Beep alle ok. Phase 8 damit abgeschlossen bis auf:

**TODO WiFi-Drop-Test (User macht das selbst, sobald er am Router
kann):** Router/2,4-GHz-Netz kurz wegnehmen, erwartet: Status-Bar
zeigt getrennt, freie Frage → "Cloud: No WiFi connection" + Beep
(kein Hänger/Reset), lokale Commands laufen weiter, nach
WLAN-Rückkehr verbindet das Gerät von selbst (Reconnect-Timer 5 s)
und Cloud-Fragen gehen wieder.

**Beobachtung (User, 2026-06-12): Display flackert leicht, während
der Lautsprecher spielt.** Verdacht: Versorgungs-Einbruch (Amp-
Spitzenströme drücken die Rail → Backlight moduliert), nicht
Software — Diagnose-Hinweise siehe Hardware-Abschnitt/Session-Notiz.

**Nächster Schritt:** Phase 10 (Function-Calling für freie
Timer-Werte, fest eingeplant). Phase 9 (AEC) bleibt optional.

---

**Nachtrag 2026-06-12 (Session 9, Fortsetzung): User-Test Phase 7 +
False-Positive-Fixes.** Der User hat mit einem frischen API-Key
getestet: **die gesprochene Antwort kam an** (E2E funktioniert,
Latenz "ein paar Sekunden" — der TTS-Request selbst braucht 3-5 s).
Zwei Probleme aus dem Test, beide behoben:

1. *Während der Wiedergabe matchte ein falsches "stop"* (eigenes
   Lautsprecher-Signal) und brach die Antwort ab.
2. *Deutsche Sätze mit "Wie..."-Anfang* matchten oft mitten im Satz
   als "mute" oder "volume up" und frassen den Cloud-Query.

Fix (geflasht und am Gerät vermessen): **per-Command-Mindest-
wahrscheinlichkeit** in `commands.c` — die kurzen Commands (stop,
mute, unmute, volume up/down) brauchen jetzt prob >= 0.25; ein
verworfener Match beendet die MultiNet-Session NICHT mehr, sondern
lauscht weiter (der Satz geht nach dem Timeout normal in die Cloud,
statt gefressen zu werden). Zusätzlich gibt der Match-Callback jetzt
die prob mit, und während SPEAKING verlangt `main.c` für "stop"
prob >= 0.30. Messwerte (per `say` am Gerät): echte Kurz-Commands
matchen mit **0.84-0.92**, die beobachteten False-Positives lagen
alle bei **0.05-0.20** — saubere Trennung mit Luft in beide
Richtungen. Tuning-Punkte sind in den Kommentaren markiert; Match-
und Reject-Logzeilen enthalten die prob.

Außerdem beobachtet: die Preview-TTS wirft sporadisch **transiente
Serverfehler** (HTTP 500 INTERNAL; vorher schon ein
finishReason=OTHER ohne Audio) — `cloud.c` macht jetzt genau einen
Sofort-Retry (nicht bei 429). **Achtung: dieser Retry ist committet,
aber noch nicht geflasht** — die Firmware auf dem Gerät ist der
Stand mit Prob-Filter, ohne TTS-Retry (beim nächsten `idf.py flash`
automatisch drauf).

**TTS ist auf User-Wunsch vorerst pausiert**: das Free-Tier-Limit
(10 Requests/Tag) ist für den Alltag zu knapp; bis eine
performantere Lösung gefunden ist (Paid Tier? anderer TTS-Anbieter?),
genügt die Text-Antwort auf dem Display. Bei erschöpftem Limit zeigt
jede Antwort die Bubble "Cloud: TTS daily limit" — falls das nervt,
wäre ein Kconfig-Schalter zum TTS-Deaktivieren der nächste Handgriff.

**Phase 7 damit abgeschlossen (User-Bestätigung 2026-06-12).**

**Nächster Schritt:** Phase 8 (Polish + Robustness) oder Phase 10
(Function-Calling für freie Timer-Werte, fest eingeplant). Zwei
offene Punkte für später:
- Solange ein gültiger Key mit Rest-Quota im sdkconfig steckt,
  spricht das Gerät automatisch wieder — falls die Antworten bewusst
  stumm bleiben sollen (User-Wunsch "nur Text"), fehlt noch ein
  Kconfig-Schalter zum TTS-Deaktivieren (5-Minuten-Änderung).
- Der "stop"-Fix (prob >= 0.30 während SPEAKING) ist gegen echte
  Wiedergabe noch unverifiziert; kostet 2 TTS-Requests, wenn TTS
  wieder aktiv wird.

---

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
deutschen/freien Saetzen.** *(Behoben in Session 9: per-Command-
Mindest-Prob 0.25 fuer die Kurz-Commands, siehe Nachtrag oben.)* Im Test matchte "Wie wird das Wetter..."
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
- **Umlaute**: Der Default-Font (Montserrat 14) enthält nur ASCII.
  Seit Phase 8 nutzen die Chat-Bubbles den Custom-Font
  `font_montserrat_14_latin1` (Latin-1 + typografische Zeichen,
  generiert per `npx lv_font_conv`, Kommandozeile im Dateikopf) —
  Status-Bar/Mic-Button bleiben beim Default-Font, weil sie dessen
  FontAwesome-Symbole (WiFi, Audio) brauchen.
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
- **LLM ist seit 2026-06-12 Cerebras, nicht mehr Gemini** (Abweichung
  von Spec Section 9.4): Gemini-Free-Tier (250 Req./Tag, 10/min) war
  im Alltag zu knapp; Cerebras Free bietet 1 Mio. Token/Tag (~8-10x).
  `main/network/llm.c`, OpenAI-kompatibles Chat-Format, Modell per
  `CONFIG_LLM_MODEL` (Default llama-3.3-70b). Phase 10 nutzt deshalb
  das OpenAI-Tool-Calling-Format. Gemini bleibt nur fuer die
  (pausierte) TTS, Spec Section 9.5.
- **Gemini-Thinking abgeschaltet** (`thinkingBudget: 0`, Phase 6,
  steht nicht in der Spec; **historisch** — seit dem Cerebras-Wechsel
  ohne Wirkung, llama-3.3-70b ist kein Reasoning-Model):
  gemini-2.5-flash "denkt" per Default, das kostete Latenz und das
  maxOutputTokens-Budget (leere Antwort mit finishReason=MAX_TOKENS).
  Achtung, falls `LLM_MODEL` auf ein Reasoning-Model wie gpt-oss-120b
  wechselt: Reasoning-Tokens zaehlen gegen max_completion_tokens.
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
- **Per-Command-Mindest-Prob statt globalem Threshold** (Session 9):
  der globale MultiNet-Threshold ist bewusst niedrig (Timer-/Volume-
  Phrasen brauchen das; seit Session 10 **0.03**, davor 0.05), aber
  die Kurz-Commands (stop/mute/unmute/volume up/down)
  verlangen prob >= 0.25 (`min_prob()` in `commands.c`); "stop"
  während SPEAKING >= 0.30 (main.c). Ein verworfener Match lässt die
  Session weiterlauschen statt sie zu beenden — deutsche Sätze gehen
  so trotzdem in die Cloud. Gemessen: echte Kurz-Commands 0.84-0.92,
  False-Positives 0.04-0.20. (Das per-Phrase-`threshold`-Feld von
  esp-sr wurde bewusst nicht genutzt: undokumentiert, ob mn7_en es
  respektiert, und es könnte die Session nicht fortsetzen.)
- **Volume-Stufen als lokale Commands** (Phase 8b, nicht in Spec
  Section 7): `CMD_VOLUME_20..100` mit expliziten Host-G2P-Phonemen
  (multinet_g2p.py) — die On-Device-G2P versagt bei "sixty"; das
  Token `SgKSTm` ist zudem im FST tot und als `SgKS Tm` gesplittet
  registriert (gleiche Fehlerklasse wie `MgNcTS`/"minutes").
  Beliebige Prozentwerte ("volume 35 percent") kommen erst mit
  Phase 10 (Function-Calling).
- **Touch-Volume-Slider in der Status-Bar** (Phase 8b): Klick auf
  die Volume-Anzeige; Panel überlagert den Chat, Auto-Hide 3 s.
  `volume_set()` ist atomar und damit auch aus dem LVGL-Task sicher.
- **Gemini-TTS-Free-Tier: 10 Requests/Tag** (verifiziert 2026-06-12,
  Quota `GenerateRequestsPerDayPerProjectPerModel-FreeTier`), nicht
  ~15 wie zuvor angenommen. HTTP 429 → eigene UI-Meldung
  "TTS daily limit"; das Tageslimit wird bewusst NICHT wiederholt
  (Phase-8-Retry greift nur bei transienten Fehlern).
- **Retry-Policy (Phase 8)**: alle Cloud-Requests bis zu 3 Versuche
  mit Backoff 1 s/2 s (`HTTP_MAX_ATTEMPTS` in `http_util.h`).
  Transient = Verbindungsfehler/429/5xx/kaputte Antwort; permanent =
  4xx (`ESP_ERR_INVALID_RESPONSE`), kein API-Key, TTS-Tageslimit.
- **TTS zur Laufzeit per Button toggelbar** (Session 11, 2026-06-16;
  ersetzt den reinen Compile-Schalter aus Phase 8): `TTS_SPEAK_REPLIES`
  ist nur noch der Startwert von `s_tts_enabled` (atomar in `cloud.c`),
  der TTS-Button unten rechts (`ui_tts_button.c`) schaltet live um. Die
  2,5 MB PSRAM-Buffer werden **lazy** beim ersten Vorlesen allokiert
  (`tts_init` aus `speak_reply` statt `cloud_init`) — aus kostet keinen
  PSRAM. Free-Tier-Limit (10/Tag) + 3-5 s Latenz bleiben der Grund,
  warum Default aus ist.
- **Stille-Guard 1000 ms** (Phase 8, `MIN_SPEECH_MS` in `main.c`):
  Captures mit weniger VAD-Sprache gehen nicht zu Whisper
  (Stille-Halluzination). Achtung bei Re-Tuning: das VAD meldet
  ~600 ms Phantom-"Speech" pro Capture (Nachhall des Wake Words),
  echte kurze Fragen liegen bei ~3700 ms.
- **vad_cache wird komplett ignoriert** (Phase 8; Phase 4-7 bekam ihn
  noch der Recorder): er überlappt mit bereits gelieferten Frames
  statt älteres Audio nachzuliefern (MultiNet-Decoder: "SfPSfT" =
  "set set"; Whisper: "Set Set Timer"). Capture startet direkt nach
  dem Wake Word, der Sprachanfang ist trotzdem vollständig.
- **Kein Touch-Kalibrierungs-Screen** (Spec-Phase-8-Punkt bewusst
  übersprungen): Touch trifft seit Phase 1 zuverlässig,
  Feinkalibrierung war in allen Tests "optional".
- **Touch-Trigger (Mic-Button) ist implementiert** (2026-06-12; in
  der Spec als Trigger vorgesehen, aber keiner Phase zugeordnet):
  LVGL-Klick -> `afe_pipeline_request_listening()` (atomares Flag)
  -> afe_fetch_task startet die normale Listening-Session ohne
  WakeNet. `wake_word_on_touch()` feuert denselben Listener wie das
  Wake Word.
- **Tool-Calling im OpenAI-Format, Single-Call** (Phase 10,
  2026-06-16): `llm_generate()` liefert `llm_result_t` (TEXT |
  TOOL_CALL). Das LLM darf set_timer/set_volume/cancel_timer/
  get_time_left aufrufen; `run_tool()` in `cloud.c` führt sie im
  network_task aus und baut die Bestätigung lokal — **bewusst kein
  zweiter OpenAI-`tool`-Message-Round-Trip** (spart Token + Latenz,
  gleiche Motivation wie der Cerebras-Wechsel). Damit kein neuer
  `CHAT_ROLE_TOOL`: History führt nur User/Model-Textpaare, der
  Bestätigungstext ist der Model-Turn (Follow-ups haben Kontext).
- **Tool-Aktionen laufen direkt im network_task** (Phase 10): keine
  Dispatch-Queue zum afe_fetch_task nötig — `kitchen_timer_start`
  (esp_timer + atomics), `volume_set` (atomar) und `canned_audio_play`
  (queued nur) sind aus jedem Task sicher. `function.arguments` ist im
  OpenAI-Format ein JSON-**String** (nicht Objekt) und wird in
  `run_tool` separat per `cJSON_Parse` ausgewertet.
- **Bestätigungs-Bubbles der Tool-Calls spiegeln
  `format_command_bubble`** (main.c): "Timer set: M:SS", "Volume:
  NN%", "Timer cancelled"/"No timer running", "Time left: M:SS" —
  Cloud-Aktionen sehen damit aus wie die lokalen MultiNet-Commands
  (ASCII, kein Latin-1-Font-Risiko).

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
