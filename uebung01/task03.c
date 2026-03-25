/*
 * Eigene Implementierungen aus <string.h>
 *
 * strlen  – im Script gegeben (zur Vollständigkeit halber hier nochmal)
 * strcpy  – a.
 * strcmp  – b.
 * memcpy  – c.
 *
 * Alle Funktionen arbeiten byteweise auf char- bzw. void-Zeigern,
 * genau wie die K&R-Referenz in Kapitel B.3 beschreibt.
 */

#include <stdio.h>
#include <stddef.h>   /* size_t */

/* -------------------------------------------------------------------------
 * strlen (aus dem Script – zur Vollständigkeit)
 *
 * Zählt Zeichen bis zur abschließenden '\0', ohne diese mitzuzählen.
 * ---------------------------------------------------------------------- */
size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p != '\0')
        p++;
    return (size_t)(p - s);
}

/* -------------------------------------------------------------------------
 * strcpy – Kopiert den String src (inklusive '\0') nach dst.
 *
 * Vorgehen: Zeichen für Zeichen kopieren, bis das '\0'-Byte übertragen
 * wurde – das passiert in der Schleifenbedingung selbst (Zuweisung liefert
 * den kopierten Wert zurück, '\0' == 0 → Schleife endet).
 *
 * Rückgabe: Zeiger auf dst (wie K&R B.3 vorschreibt).
 * Achtung:  dst muss groß genug für src sein (keine Längenprüfung!).
 * ---------------------------------------------------------------------- */
char *my_strcpy(char *dst, const char *src) {
    char *start = dst;              /* Zeiger auf Anfang merken für return */

    while ((*dst++ = *src++) != '\0')
        ;                           /* Schleifenrumpf leer – alles passiert
                                       in der Bedingung:
                                       1. *src nach *dst kopieren
                                       2. beide Zeiger vorrücken
                                       3. Wert prüfen → '\0' beendet Schleife */
    return start;
}

/* -------------------------------------------------------------------------
 * strcmp – Vergleicht zwei Strings lexikografisch.
 *
 * Vorgehen: Zeichen für Zeichen vergleichen, solange beide gleich sind
 * und noch kein String zu Ende ist.  Am Ende (oder bei erstem Unterschied)
 * liefert die Differenz der unsigned char-Werte:
 *   < 0  wenn s1 lexikografisch vor s2 liegt
 *   = 0  wenn beide Strings identisch sind
 *   > 0  wenn s1 lexikografisch nach s2 liegt
 *
 * Wichtig: Vergleich als *unsigned* char, damit Zeichen > 127 (z.B. Umlaute)
 * korrekt geordnet werden (K&R B.3 schreibt unsigned char explizit vor).
 * ---------------------------------------------------------------------- */
int my_strcmp(const char *s1, const char *s2) {
    /* Cast auf unsigned char: verhindert falsche Vorzeichen bei > 127 */
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    while (*p1 == *p2 && *p1 != '\0') {
        p1++;
        p2++;
    }
    /* *p1 - *p2:  0 bei Gleichheit, negativ/positiv bei Unterschied */
    return (int)(*p1 - *p2);
}

/* -------------------------------------------------------------------------
 * memcpy – Kopiert n Bytes von src nach dst.
 *
 * Unterschied zu strcpy: kein '\0'-Abbruch, Länge n wird explizit angegeben.
 * Damit können beliebige Binärdaten (nicht nur Strings) kopiert werden.
 *
 * Vorgehen: Quell- und Ziel-Zeiger als char* behandeln (1 Byte pro Schritt),
 * genau n Bytes übertragen.
 *
 * Rückgabe: Zeiger auf dst (wie K&R B.3 vorschreibt).
 * Achtung:  src und dst dürfen sich NICHT überlappen (dafür gibt es memmove).
 * ---------------------------------------------------------------------- */
void *my_memcpy(void *dst, const void *src, size_t n) {
          char *d = (char *)dst;
    const char *s = (const char *)src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

/* -------------------------------------------------------------------------
 * Einfacher Test
 * ---------------------------------------------------------------------- */
int main(void) {
    /* --- strcpy --- */
    char buf[64];
    my_strcpy(buf, "Hallo ESP32");
    printf("strcpy  : \"%s\"\n", buf);

    /* --- strcmp --- */
    printf("strcmp  : \"abc\" vs \"abc\" → %d (erwartet 0)\n",  my_strcmp("abc", "abc"));
    printf("strcmp  : \"abc\" vs \"abd\" → %d (erwartet < 0)\n", my_strcmp("abc", "abd"));
    printf("strcmp  : \"abd\" vs \"abc\" → %d (erwartet > 0)\n", my_strcmp("abd", "abc"));
    printf("strcmp  : \"\"    vs \"a\"   → %d (erwartet < 0)\n", my_strcmp("",   "a"));

    /* --- memcpy --- */
    int src[] = {1, 2, 3, 4, 5};
    int dst[5];
    my_memcpy(dst, src, sizeof(src));
    printf("memcpy  : {%d, %d, %d, %d, %d} (erwartet {1,2,3,4,5})\n",
           dst[0], dst[1], dst[2], dst[3], dst[4]);

    /* --- strlen (Referenz) --- */
    printf("strlen  : \"%s\" → %zu (erwartet 11)\n", "Hallo ESP32",
           my_strlen("Hallo ESP32"));

    return 0;
}
