#pragma once

#include <stdbool.h>

#include "esp_err.h"

// DS18B20-Temperatursensor (1-Wire, GPIO 48). Messung laeuft asynchron
// in einem periodischen esp_timer; temperature_get() liefert sofort
// den zuletzt gecachten Wert ohne Bus-I/O.

#define TEMP_ONEWIRE_GPIO 48

// Startet Bus und esp_timer. Erste Messung nach ~1 s.
esp_err_t temperature_init(void);

// Letzter gecachter Wert in Grad Celsius. Nur gueltig, wenn
// temperature_valid() true; sonst undefiniert.
float temperature_get(void);

// false, bis zur ersten erfolgreichen Messung und nach Bus/CRC-Fehlern.
bool temperature_valid(void);
