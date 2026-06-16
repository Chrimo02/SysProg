#pragma once

#include <stdbool.h>

// WLAN-Setup-Portal: spannt einen SoftAP "MiniLingo-Setup" auf und
// serviert ein kleines Web-Formular (http://192.168.4.1), über das
// SSID/Passwort und die API-Keys ins NVS (config.c) geschrieben werden.
// Nach dem Speichern rebootet das Gerät mit den neuen Werten. Damit
// braucht ein Netz-/Key-Wechsel kein USB-Reflashen mehr.

// Startet AP + HTTP-Server (idempotent). Aufruf aus dem LVGL-Task
// (UI-Trigger) oder beim Erststart (keine SSID provisioniert).
void provisioning_start(void);

bool provisioning_is_active(void);
