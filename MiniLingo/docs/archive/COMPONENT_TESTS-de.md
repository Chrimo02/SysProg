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

## Display: ILI9341 (SPI)

**Datei:** `main/display_test.c`

### Verkabelung

| Display Pin | ESP32-S3 Pin | Hinweis |
|-------------|--------------|---------|
| VCC  | 3V3    | |
| GND  | GND    | |
| CS   | GPIO10 | |
| RESET| GPIO9  | |
| DC/RS| GPIO8  | |
| SDI/MOSI | GPIO11 | |
| SCK  | GPIO12 | |
| LED  | 3V3    | immer an |
| SDO/MISO | GPIO13 | |

GPIOs wurden bewusst frei von den Mic-/Amp-Tests (4/5/6, 15/16/17)
gewählt, sodass der XPT2046-Touch (siehe unten) sich SCK/MOSI/MISO mit
dem Display teilen kann (eigener CS-Pin).

### Implementierung

- SPI2_HOST wird über `spi_bus_initialize` konfiguriert.
- Das ILI9341-Panel wird über die Managed Component
  `espressif/esp_lcd_ili9341` (esp_lcd-Treiber, kein Arduino) mit
  16-Bit RGB565, Auflösung 240x320 initialisiert.
- Reset, Init und Display-On werden über `esp_lcd_panel_*`-Funktionen
  ausgeführt.
- In einer Endlosschleife wird der Bildschirm nacheinander mit
  Rot/Grün/Blau/Weiß gefüllt (`esp_lcd_panel_draw_bitmap`, zeilenweise),
  je 2 Sekunden Anzeigedauer.

### Testergebnis

Alle vier Farben wurden korrekt und vollflächig auf dem Display
angezeigt. ✅ Erfolgreich getestet am 2026-06-10.

## Touch: XPT2046 (SPI)

**Datei:** `main/touch_test.c`

### Verkabelung

| Touch (XPT2046) Pin | ESP32-S3 Pin | Hinweis |
|----------------------|--------------|---------|
| T_CLK | GPIO12 (=SCK)  | gemeinsamer SPI-Bus mit Display |
| T_CS  | GPIO14 | eigener CS |
| T_DIN | GPIO11 (=MOSI) | gemeinsamer SPI-Bus mit Display |
| T_DO  | GPIO13 (=MISO) | gemeinsamer SPI-Bus mit Display |
| T_IRQ | nicht angeschlossen | Polling statt Interrupt |

### Implementierung

- SPI2_HOST wird mit denselben Pins wie das Display konfiguriert
  (`SPI_DMA_DISABLED`, da nur kurze 3-Byte-Transfers nötig sind),
  der XPT2046 erhält ein eigenes `spi_device` mit CS=GPIO14.
- In einer Endlosschleife werden per `spi_device_polling_transmit`
  die Steuerbytes 0x90/0xD0/0xB0/0xC0 gesendet, um X-, Y-, Z1- und
  Z2-Rohwerte (12 Bit) auszulesen.
- T_IRQ ist nicht angeschlossen; stattdessen wird alle 50 ms gepollt
  und nur bei Z1 > 100 (Druck erkannt) eine Zeile per `ESP_LOGI`
  ausgegeben.

### Testergebnis (Touch)

Beim Berühren des Displays wurden plausible X/Y/Z-Rohwerte ausgegeben,
die sich mit der Berührposition änderten. ✅ Erfolgreich getestet am
2026-06-10.
