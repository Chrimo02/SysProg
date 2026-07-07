#pragma once

#include <stdbool.h>  // [C] Standard-Header | [C] liefert den Typ bool sowie true/false

// WLAN-Setup-Portal: spannt einen SoftAP "MiniLingo-Setup" auf und
// serviert ein kleines Web-Formular (http://192.168.4.1), über das
// SSID/Passwort und die API-Keys ins NVS (config.c) geschrieben werden.
// Nach dem Speichern rebootet das Gerät mit den neuen Werten. Damit
// braucht ein Netz-/Key-Wechsel kein USB-Reflashen mehr.

// Startet AP + HTTP-Server (idempotent). Aufruf aus dem LVGL-Task
// (UI-Trigger) oder beim Erststart (keine SSID provisioniert).
//
// Öffentliche Funktion: startet das Provisioning (SoftAP + HTTP-Server),
// falls es nicht schon läuft. Wird vom UI-Task oder beim Erststart
// aufgerufen, damit der Nutzer WLAN/API-Keys per Browser eintragen kann.
void provisioning_start(void);  // [C] Funktionsdeklaration ohne Rückgabewert, kein Parameter | [HW] startet SoftAP + HTTP-Server für das Setup-Portal

// Öffentliche Abfragefunktion: liefert zurück, ob das Provisioning-Portal
// aktuell aktiv ist (SoftAP + HTTP-Server laufen). Wird z.B. von der UI
// genutzt, um den Status anzuzeigen oder Doppelstarts zu vermeiden.
bool provisioning_is_active(void);  // [C] Deklaration, Rückgabetyp bool | [Logik] Abfrage des aktuellen Provisioning-Zustands
