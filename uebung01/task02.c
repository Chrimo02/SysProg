/*
 * Fakultät großer Zahlen (bis 100.000) auf einer 32-Bit-Plattform
 *
 * Problem: 100000! hat ~1.517.000 Bits – kein nativer Typ reicht aus.
 * Lösung:  Big-Integer als Array von 32-Bit-Wörtern in Basis 2^32.
 *          Jedes Element speichert 32 Bit des Ergebnisses (little-endian).
 *          100000! benötigt ca. 47.397 solcher Wörter (50.000 mit Puffer).
 *
 * Auf 32-Bit-Plattform:
 *   unsigned long       = 32 Bit  (Speicherelemente)
 *   unsigned long long  = 64 Bit  (temporäres Carry bei Multiplikation)
 *
 * Ausgabe des Ergebnisses in hexadezimaler Form in die Datei "result.hex".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Basis-Typen: u32 für die Ziffern unserer Big-Integer-Zahl,
   u64 nur intern beim Multiplizieren, damit der Übertrag (Carry) nicht überläuft */
typedef unsigned long       u32;   /* 32 Bit auf 32-Bit-Platform */
typedef unsigned long long  u64;   /* 64 Bit für Carry           */

/* Maximale Anzahl an 32-Bit-Blöcken im Ergebnis-Array.
   100000! braucht ~47.397 Blöcke; 50.000 gibt uns etwas Sicherheitspuffer. */
#define MAX_WORDS 50000

/* Vorwärtsdeklarationen der Hilfsfunktionen */
void bignum_init(u32 *num, int *size);
void bignum_multiply(u32 *num, int *size, u32 factor);
double elapsed_ms(struct timespec start, struct timespec end);

int main() {
    int n;
    printf("n eingeben (0 <= n <= 100000): ");
    scanf("%d", &n);

    /* Eingabe prüfen – n muss im erlaubten Bereich liegen */
    if (n < 0 || n > 100000) {
        fprintf(stderr, "Fehler: n muss zwischen 0 und 100000 liegen.\n");
        return 1;
    }

    /* Speicher für die Big-Integer-Zahl anfordern.
       calloc initialisiert alle Bytes auf 0, was wichtig ist,
       da ungenutzte Blöcke am Anfang nicht zufälligen Müll enthalten dürfen. */
    u32 *result = (u32 *)calloc(MAX_WORDS, sizeof(u32));
    if (!result) {
        fprintf(stderr, "Fehler: Speicher konnte nicht alloziert werden.\n");
        return 1;
    }

    /* Big-Integer auf den Startwert 1 setzen (neutrale Element der Multiplikation) */
    int word_count;
    bignum_init(result, &word_count);

    /* Zeitmessung: CLOCK_MONOTONIC läuft gleichmäßig ohne Sprünge (z.B. durch Sommerzeit),
       daher gut geeignet für Laufzeitmessungen */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Kernschleife: result = 1 * 2 * 3 * ... * n  (d.h. n!) */
    for (int i = 2; i <= n; i++) {
        bignum_multiply(result, &word_count, (u32)i);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double ms = elapsed_ms(start, end);
    printf("Rechenzeit: %.3f ms\n", ms);
    printf("Ergebnis:   %d 32-Bit-Wörter (~%d Bit)\n", word_count, word_count * 32);

    /* Ergebnis als Hex-String in Datei schreiben */
    FILE *f = fopen("result.hex", "w");
    if (!f) {
        fprintf(stderr, "Fehler: Datei 'result.hex' konnte nicht geöffnet werden.\n");
        free(result);
        return 1;
    }

    fprintf(f, "%d! =\n0x", n);

    /* Ausgabe von links nach rechts (höchstes Wort zuerst = big-endian für Menschen lesbar).
       Das erste (höchste) Wort wird ohne führende Nullen ausgegeben,
       alle weiteren immer mit 8 Hex-Ziffern aufgefüllt, damit die Stellen stimmen. */
    fprintf(f, "%lX", (unsigned long)result[word_count - 1]);
    for (int i = word_count - 2; i >= 0; i--) {
        fprintf(f, "%08lX", (unsigned long)result[i]);
    }
    fprintf(f, "\n");
    fclose(f);

    printf("Ergebnis in 'result.hex' gespeichert.\n");

    free(result);
    return 0;
}

/* Setzt den Big-Integer auf den Startwert 1.
   Nur das erste Element (niedrigstes Wort) wird auf 1 gesetzt,
   der Rest bleibt 0. word_count = 1, weil wir genau ein Wort belegt haben. */
void bignum_init(u32 *num, int *size) {
    num[0] = 1;
    *size  = 1;
}

/* Multipliziert den Big-Integer num mit einem einfachen 32-Bit-Faktor.
   Das Verfahren entspricht der schriftlichen Multiplikation:
   Jede "Ziffer" (32-Bit-Block) wird mit dem Faktor multipliziert,
   ein möglicher Überlauf (Carry) wird ins nächste Wort weitergegeben.
   Da num[i] und factor je bis zu 32 Bit groß sind, kann ihr Produkt
   bis zu 64 Bit groß werden – deshalb rechnen wir intern mit u64. */
void bignum_multiply(u32 *num, int *size, u32 factor) {
    u64 carry = 0;
    for (int i = 0; i < *size; i++) {
        u64 product = (u64)num[i] * factor + carry;  /* max. 64 Bit, kein Überlauf */
        num[i] = (u32)(product & 0xFFFFFFFFUL);       /* untere 32 Bit behalten    */
        carry  = product >> 32;                        /* obere 32 Bit weitergeben  */
    }
    /* Falls nach dem letzten Wort noch ein Carry übrig ist,
       brauchen wir ein neues Wort – die Zahl wächst um einen Block. */
    if (carry > 0) {
        num[(*size)++] = (u32)carry;
    }
}

/* Berechnet die vergangene Zeit in Millisekunden zwischen zwei Messpunkten.
   tv_sec  = volle Sekunden,  tv_nsec = Nanosekunden-Anteil (0–999.999.999) */
double elapsed_ms(struct timespec start, struct timespec end) {
    return (end.tv_sec  - start.tv_sec)  * 1000.0   /* Sekunden → ms    */
         + (end.tv_nsec - start.tv_nsec) / 1e6;      /* Nanosekunden → ms */
}
