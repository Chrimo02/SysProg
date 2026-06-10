# MiniLingo – Komponententests

Dieses Dokument beschreibt die Hardware-Komponententests, die vor dem
Zusammenbau des eigentlichen MiniLingo-Projekts auf einem ESP32-S3
(MON16R8) durchgeführt wurden. Jeder Abschnitt enthält die Verkabelung,
den Zweck des Tests sowie eine Erklärung der implementierten Firmware.

## Mikrofon: INMP441 (I2S)

**Datei:** `main/mic_test.c`

### Verkabelung

| INMP441 Pin | ESP32-S3 Pin |
|-------------|--------------|
| GND         | GND          |
| VDD         | 3V3          |
| SD          | GPIO6        |
| SCK         | GPIO5        |
| WS          | GPIO4        |
| L/R         | GND          |

L/R liegt auf GND, wodurch das INMP441 seine Daten im linken Kanal des
I2S-Frames sendet.

### Implementierung

- I2S-Peripherie wird über die ESP-IDF v5.x Standard-API
  (`driver/i2s_std.h`) im Master-RX-Modus initialisiert
  (Philips-Format, 32-Bit, Mono, linker Slot).
- Sample-Rate: 16 kHz.
- In einer Endlosschleife werden Blöcke von 512 Samples gelesen.
- Da das INMP441 nur 24 Bit gültige Daten liefert (linksbündig im
  32-Bit-Frame), werden die Samples per `>> 8` auf 24 Bit normalisiert.
- Pro Block werden RMS- und Peak-Amplitude berechnet und alle ~100 ms
  über `ESP_LOGI` ausgegeben.
- Der Sample-Puffer ist `static`, um einen Stack-Overflow im
  `main`-Task (Standardgröße 3584 Byte) zu vermeiden.

### Testergebnis

Bei Stille liegen RMS/Peak nahe 0, bei Sprache/Klatschen steigen die
Werte deutlich an (RMS im Bereich mehrerer 10.000 bis >100.000, je nach
Lautstärke). ✅ Erfolgreich getestet am 2026-06-10.

## Verstärker: MAX98357A + 2x 4Ω/3W Lautsprecher

**Datei:** `main/amp_test.c`

### Verkabelung

| MAX98357A Pin | ESP32-S3 Pin | Hinweis |
|----------------|--------------|---------|
| VIN  | 3V3 | Versorgung |
| GND  | GND | |
| SD   | 3V3 | Muss High sein, sonst Shutdown (intern auf GND gezogen) |
| GAIN | offen | = 9 dB Standardverstärkung |
| BCLK | GPIO15 | I2S Bit Clock |
| LRC  | GPIO16 | I2S Word Select (L/R Clock) |
| DIN  | GPIO17 | I2S Data In |

Andere GPIOs als beim Mikrofon (4/5/6), damit beide Peripherien später
unabhängig (Mic auf I2S0, Amp auf I2S1) gleichzeitig laufen können.

Die beiden 4Ω-Lautsprecher wurden **in Reihe** geschaltet (Amp(+) →
Speaker1(+), Speaker1(-) → Speaker2(+), Speaker2(-) → Amp(-)), um auf
8Ω Gesamtlast zu kommen (parallel wären 2Ω zu niedrig für den
MAX98357A).

### Implementierung

- I2S-Peripherie (I2S_NUM_1) wird im Master-TX-Modus initialisiert
  (Philips-Format, 16-Bit, Mono, linker Slot, 44.1 kHz).
- Ein 1 kHz-Sinuston wird einmalig als Lookup-Tabelle (256 Samples)
  berechnet und danach endlos über `i2s_channel_write` ausgegeben.
- Sample-Puffer ist `static`, um Stack-Probleme wie beim Mic-Test zu
  vermeiden.

### Testergebnis

1 kHz-Ton war über beide in Reihe geschalteten Lautsprecher klar
hörbar. ✅ Erfolgreich getestet am 2026-06-10.
