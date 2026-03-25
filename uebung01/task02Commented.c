 /*
 * ============================================================
 *  Fakultät großer Zahlen – Big-Integer-Implementierung in C
 * ============================================================
 *
 * PROBLEM:
 *   Normale C-Datentypen sind zu klein für große Fakultäten:
 *     - int           (32 Bit) → überläuft bereits bei 13!
 *     - unsigned long (32 Bit) → überläuft bei 13!
 *     - unsigned long long (64 Bit) → überläuft bei 21!
 *   100.000! hat ca. 1.517.000 Bit – kein nativer Typ reicht aus.
 *
 * LÖSUNG: Big-Integer als Array von 32-Bit-Blöcken
 *   Wir speichern die Zahl nicht in einem einzigen Typ,
 *   sondern in einem Array aus vielen 32-Bit-Blöcken.
 *   Jeder Block ist eine "Ziffer" in Basis 2^32.
 *   Das Array ist little-endian:
 *     result[0]  = niedrigstes (kleinstes) Stück der Zahl
 *     result[N]  = höchstes (größtes) Stück der Zahl
 *
 * BEISPIEL – wie die Zahl 2^33 = 8.589.934.592 gespeichert wird:
 *   result[0] = 0x00000000  (untere 32 Bit = 0)
 *   result[1] = 0x00000002  (obere 32 Bit = 2)
 *   word_count = 2
 *
 * SPEICHERBEDARF:
 *   100.000! benötigt ca. 47.397 Blöcke à 32 Bit.
 *   Wir reservieren 50.000 Blöcke (= 200.000 Byte ≈ 200 KB) als Sicherheitspuffer.
 *
 * AUSGABE:
 *   Das Ergebnis wird hexadezimal in die Datei "result.hex" geschrieben.
 *
 * PLATTFORM: 32-Bit (unsigned long = 32 Bit, unsigned long long = 64 Bit)
 */

#include <stdio.h>   /* printf, fprintf, scanf, fopen, fclose, FILE */
#include <stdlib.h>  /* calloc, free, exit                           */
#include <string.h>  /* (indirekt genutzt, z.B. für Speicherops)    */
#include <time.h>    /* clock_gettime, struct timespec, CLOCK_MONOTONIC */

/* ------------------------------------------------------------------
 * TYPDEFINITIONEN
 * ------------------------------------------------------------------
 * "typedef" erstellt einen Spitznamen für einen bestehenden Typ.
 * Das macht den Code lesbarer und plattformunabhängiger.
 *
 * u32 = 32-Bit-Ganzzahl ohne Vorzeichen
 *       Wertebereich: 0 bis 4.294.967.295 (= 2^32 - 1)
 *       Wird für die Array-Blöcke unserer Big-Integer-Zahl genutzt.
 *       "unsigned" = kein Vorzeichen → kein negatives Vorzeichen-Bit,
 *       dadurch ist der positive Bereich doppelt so groß wie bei signed.
 *
 * u64 = 64-Bit-Ganzzahl ohne Vorzeichen
 *       Wertebereich: 0 bis 18.446.744.073.709.551.615 (= 2^64 - 1)
 *       Wird NUR intern bei der Multiplikation genutzt, damit der
 *       Übertrag (Carry) nicht überläuft:
 *         u32 * u32 kann bis zu ~2^64 groß werden → braucht u64!
 * ------------------------------------------------------------------ */
typedef unsigned long       u32;   /* 32 Bit auf 32-Bit-Plattform */
typedef unsigned long long  u64;   /* 64 Bit für Zwischenergebnisse */

/* ------------------------------------------------------------------
 * KONSTANTE
 * ------------------------------------------------------------------
 * #define ist kein Variablentyp und reserviert keinen Speicher.
 * Der Präprozessor ersetzt vor der Kompilierung jedes "MAX_WORDS"
 * im Code durch die Zahl 50000. Es ist nur ein benannter Wert.
 *
 * 100.000! braucht ~47.397 Blöcke.
 * 50.000 Blöcke gibt uns ~2.600 Blöcke Sicherheitspuffer.
 * ------------------------------------------------------------------ */
#define MAX_WORDS 50000

/* ------------------------------------------------------------------
 * VORWÄRTSDEKLARATIONEN
 * ------------------------------------------------------------------
 * C verlangt, dass Funktionen bekannt sind, bevor sie aufgerufen werden.
 * Da bignum_init() und bignum_multiply() nach main() definiert sind,
 * müssen wir hier ihre Signaturen vorab ankündigen.
 * Der Compiler weiß dann: "Diese Funktion existiert, Definition folgt."
 * ------------------------------------------------------------------ */
void bignum_init(u32 *num, int *size);
void bignum_multiply(u32 *num, int *size, u32 factor);
double elapsed_ms(struct timespec start, struct timespec end);


/* ==================================================================
 * HAUPTFUNKTION
 * ================================================================== */
int main() {

    /* --------------------------------------------------------------
     * SCHRITT 1: Eingabe einlesen
     * --------------------------------------------------------------
     * int n → normale 32-Bit-Ganzzahl mit Vorzeichen (-2 Mrd. bis +2 Mrd.)
     * scanf("%d", &n):
     *   - "%d" = Formatangabe: lies eine Dezimalzahl
     *   - "&n"  = Adresse von n (& = "Adresse von")
     *     scanf braucht die Speicheradresse, weil es den Wert direkt
     *     in die Variable schreiben muss (call by reference in C).
     * -------------------------------------------------------------- */
    int n;
    printf("n eingeben (0 <= n <= 100000): ");
    scanf("%d", &n);

    /* --------------------------------------------------------------
     * SCHRITT 2: Eingabe validieren
     * --------------------------------------------------------------
     * Falls n außerhalb des erlaubten Bereichs liegt, Programm abbrechen.
     *
     * fprintf(stderr, ...):
     *   - stderr = Fehlerausgabe-Kanal (getrennt von stdout)
     *   - Vorteil: Fehlermeldungen können separat umgeleitet werden,
     *     z.B. "programm 2> fehler.log" in der Shell
     *
     * return 1:
     *   - Beendet main() und damit das Programm.
     *   - Rückgabewert 0 = Erfolg, alles andere (z.B. 1) = Fehler.
     *   - Shells werten diesen Wert aus (z.B. in Skripten).
     * -------------------------------------------------------------- */
    if (n < 0 || n > 100000) {
        fprintf(stderr, "Fehler: n muss zwischen 0 und 100000 liegen.\n");
        return 1;
    }

    /* --------------------------------------------------------------
     * SCHRITT 3: Speicher für die Big-Integer-Zahl reservieren
     * --------------------------------------------------------------
     * u32 *result:
     *   - Das ist ein ZEIGER auf u32.
     *   - Ein Zeiger ist eine Variable, die eine Speicheradresse enthält.
     *   - result zeigt auf den Anfang unseres Arrays im Heap.
     *   - Der Heap ist ein Speicherbereich, der zur Laufzeit reserviert
     *     werden kann (im Gegensatz zum Stack, der beim Aufruf fix ist).
     *
     * calloc(MAX_WORDS, sizeof(u32)):
     *   - Reserviert MAX_WORDS * sizeof(u32) = 50.000 * 4 = 200.000 Byte
     *   - sizeof(u32) = 4, weil u32 = 32 Bit = 4 Byte
     *   - WICHTIG: calloc setzt alle Bytes auf 0 (im Gegensatz zu malloc!)
     *     Das ist entscheidend, weil ungenutzte Blöcke wirklich 0 sein müssen.
     *   - Rückgabewert: void* (typloser Zeiger auf den reservierten Speicher)
     *
     * (u32 *) – der Cast:
     *   - calloc gibt void* zurück, wir brauchen aber u32*.
     *   - Der Cast konvertiert den Zeigertyp explizit.
     *   - In C89/C90 war das nötig, in C99+ optional, aber guter Stil.
     * -------------------------------------------------------------- */
    u32 *result = (u32 *)calloc(MAX_WORDS, sizeof(u32));

    /* --------------------------------------------------------------
     * SCHRITT 4: Prüfen ob calloc erfolgreich war
     * --------------------------------------------------------------
     * calloc gibt NULL zurück wenn:
     *   - nicht genug freier RAM vorhanden ist
     *   - das Betriebssystem den Speicher verweigert
     *
     * NULL ist eine Konstante für den "leeren" Zeiger (Adresse 0).
     * !result ist true wenn result == NULL (also kein Speicher da).
     *
     * Ohne diese Prüfung: Wenn wir später auf result[i] zugreifen,
     * würde das Programm abstürzen (Segmentation Fault), weil wir
     * auf Adresse 0 schreiben würden – ein verbotener Speicherbereich.
     * -------------------------------------------------------------- */
    if (!result) {
        fprintf(stderr, "Fehler: Speicher konnte nicht alloziert werden.\n");
        return 1;
    }

    /* --------------------------------------------------------------
     * SCHRITT 5: Big-Integer auf den Startwert 1 setzen
     * --------------------------------------------------------------
     * int word_count:
     *   - Zählt, wie viele Blöcke des Arrays aktuell belegt sind.
     *   - Am Anfang brauchen wir nur 1 Block (die Zahl = 1).
     *   - Wächst automatisch mit der Zahl, wenn bignum_multiply neue
     *     Blöcke braucht.
     *
     * &word_count → Adresse übergeben, damit bignum_init den Wert
     * von word_count direkt verändern kann (call by reference).
     * -------------------------------------------------------------- */
    int word_count;
    bignum_init(result, &word_count);

    /* --------------------------------------------------------------
     * SCHRITT 6: Zeitmessung starten
     * --------------------------------------------------------------
     * struct timespec:
     *   Ein zusammengesetzter Datentyp (Struktur) mit zwei Feldern:
     *     tv_sec  → volle Sekunden seit dem 1.1.1970 (Unix-Timestamp)
     *     tv_nsec → Nanosekunden-Anteil (0 bis 999.999.999)
     *
     * clock_gettime(CLOCK_MONOTONIC, &start):
     *   - Speichert den aktuellen Zeitstempel in "start"
     *   - CLOCK_MONOTONIC = eine Uhr, die NIEMALS zurückläuft.
     *     Selbst wenn die Systemzeit geändert wird (z.B. Zeitumstellung,
     *     NTP-Korrektur), läuft CLOCK_MONOTONIC immer gleichmäßig weiter.
     *     Ideal für Laufzeitmessungen (im Gegensatz zu CLOCK_REALTIME).
     * -------------------------------------------------------------- */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* --------------------------------------------------------------
     * SCHRITT 7: Kernschleife – Fakultät berechnen: 1 * 2 * 3 * ... * n
     * --------------------------------------------------------------
     * Schleife startet bei 2, weil × 1 das Ergebnis nicht verändert.
     *
     * (u32)i – der Cast:
     *   i ist ein int (mit Vorzeichen), bignum_multiply erwartet u32 (ohne).
     *   Der Cast ist hier sicher, weil i immer > 0 und <= 100.000 ist –
     *   also nie negativ, und 100.000 < 2^32.
     *
     * Bei jedem Schritt wächst result im Speicher:
     *   Nach × 2   → result ≈ 1 Block
     *   Nach × 100 → result ≈ 1-2 Blöcke
     *   Nach × 100.000 → result ≈ 47.397 Blöcke
     * -------------------------------------------------------------- */
    for (int i = 2; i <= n; i++) {
        bignum_multiply(result, &word_count, (u32)i);
    }

    /* --------------------------------------------------------------
     * SCHRITT 8: Zeitmessung stoppen und ausgeben
     * -------------------------------------------------------------- */
    clock_gettime(CLOCK_MONOTONIC, &end);

    /* double = 64-Bit-Fließkommazahl, nötig für Millisekunden mit
     * Nachkommastellen. "%.3f" = 3 Nachkommastellen. */
    double ms = elapsed_ms(start, end);
    printf("Rechenzeit: %.3f ms\n", ms);

    /* word_count * 32 = Gesamtanzahl der Bits im Ergebnis.
     * "%d" = Platzhalter für int in printf. */
    printf("Ergebnis:   %d 32-Bit-Woerter (~%d Bit)\n",
           word_count, word_count * 32);

    /* --------------------------------------------------------------
     * SCHRITT 9: Ergebnis als Hex in Datei schreiben
     * --------------------------------------------------------------
     * FILE *f:
     *   Ein Zeiger auf eine FILE-Struktur. Diese Struktur enthält
     *   intern alle Infos über die offene Datei (Puffer, Position, etc.).
     *   fopen gibt die Adresse dieser Struktur zurück.
     *
     * fopen("result.hex", "w"):
     *   - "w" = write-Modus: Datei neu erstellen oder überschreiben
     *   - Andere Modi: "r" = lesen, "a" = anhängen, "rb" = binär lesen
     *   - Rückgabe: NULL wenn die Datei nicht geöffnet werden konnte
     * -------------------------------------------------------------- */
    FILE *f = fopen("result.hex", "w");
    if (!f) {
        fprintf(stderr, "Fehler: Datei 'result.hex' konnte nicht geoeffnet werden.\n");
        free(result);   /* Speicher immer freigeben, auch im Fehlerfall! */
        return 1;
    }

    /* Kopfzeile in die Datei schreiben (z.B. "5! =" ) */
    fprintf(f, "%d! =\n0x", n);

    /* --------------------------------------------------------------
     * Hexadezimale Ausgabe – höchstes Wort zuerst (big-endian für
     * Menschen lesbar, auch wenn das Array little-endian ist).
     *
     * result[word_count - 1] = höchster belegter Block
     * "%lX" = unsigned long als Hex, OHNE führende Nullen
     *          Das ist korrekt für das erste (höchste) Wort.
     * -------------------------------------------------------------- */
    fprintf(f, "%lX", (unsigned long)result[word_count - 1]);

    /* Alle weiteren Blöcke von hoch nach niedrig ausgeben.
     * "%08lX" = immer GENAU 8 Hex-Zeichen, mit führenden Nullen!
     * WARUM wichtig: Würden wir führende Nullen weglassen, wäre die
     * Zahl falsch. Beispiel: Block = 0x000000FF
     *   Ohne führende Nullen: "FF"        → Zahl zu klein!
     *   Mit "%08lX":          "000000FF"  → korrekt!
     * Jeder Block MUSS genau 8 Hex-Zeichen haben (32 Bit / 4 = 8). */
    for (int i = word_count - 2; i >= 0; i--) {
        fprintf(f, "%08lX", (unsigned long)result[i]);
    }
    fprintf(f, "\n");
    fclose(f);   /* Datei schließen → Puffer leeren, Handle freigeben */

    printf("Ergebnis in 'result.hex' gespeichert.\n");

    /* --------------------------------------------------------------
     * SCHRITT 10: Speicher freigeben
     * --------------------------------------------------------------
     * Jeder mit calloc/malloc reservierte Speicher MUSS mit free()
     * wieder freigegeben werden. Sonst entsteht ein Memory Leak:
     * Der Speicher bleibt belegt bis das Programm endet.
     * Bei kurzen Programmen kein Problem, bei langen (Daemons, Server)
     * kann es zum Absturz führen.
     * -------------------------------------------------------------- */
    free(result);
    return 0;   /* 0 = Programm erfolgreich beendet */
}


/* ==================================================================
 * bignum_init() – Big-Integer auf Startwert 1 setzen
 * ==================================================================
 *
 * PARAMETER:
 *   u32 *num  → Zeiger auf das Array (nicht das Array selbst!).
 *               In C werden Arrays als Zeiger auf ihr erstes Element
 *               übergeben. num[0] = *(num+0), num[1] = *(num+1), etc.
 *   int *size → Zeiger auf word_count in main(). Durch den Zeiger
 *               kann diese Funktion word_count direkt verändern.
 *               *size = 1 schreibt den Wert 1 an die Adresse von size.
 *
 * ERGEBNIS nach Aufruf:
 *   num[0] = 1  → die Zahl "1" im ersten Block
 *   *size  = 1  → genau 1 Block ist belegt
 *   num[1..49999] = 0  (wurden von calloc schon auf 0 gesetzt)
 * ================================================================== */
void bignum_init(u32 *num, int *size) {
    num[0] = 1;    /* Ersten Block auf 1 setzen (= Startwert der Fakultät) */
    *size  = 1;    /* Wir belegen genau 1 Block; *size schreibt über Zeiger */
}


/* ==================================================================
 * bignum_multiply() – Big-Integer mit einer 32-Bit-Zahl multiplizieren
 * ==================================================================
 *
 * IDEE: Schriftliche Multiplikation, nur in Basis 2^32 statt Basis 10.
 *   In Basis 10: 123 × 4 → (3×4=12, schreib 2 carry 1), (2×4+1=9), (1×4=4) → 492
 *   Hier:        [A, B, C] × factor → selbe Logik, aber mit 32-Bit-Blöcken
 *
 * PARAMETER:
 *   u32 *num    → Zeiger auf das Big-Integer-Array
 *   int *size   → Zeiger auf word_count (kann wachsen!)
 *   u32 factor  → der Multiplikator (einfache 32-Bit-Zahl: 2, 3, 4, ...)
 *
 * WARUM u64 für carry und product?
 *   num[i] kann maximal 2^32 - 1 ≈ 4,3 Mrd. sein.
 *   factor kann maximal 100.000 sein.
 *   Produkt maximal: (2^32-1) × 100.000 ≈ 4,3 × 10^14 → braucht 48 Bit.
 *   Dazu noch carry (max. ebenfalls ~48 Bit) → sicher in u64 (64 Bit).
 *   Würden wir u32 nutzen, würde das Produkt überlaufen!
 * ================================================================== */
void bignum_multiply(u32 *num, int *size, u32 factor) {

    /* carry = Übertrag vom vorherigen Block, startet immer bei 0 */
    u64 carry = 0;

    /* Jeden belegten Block von unten (niedrig) nach oben (hoch) durchgehen */
    for (int i = 0; i < *size; i++) {

        /* --------------------------------------------------------
         * KERNOPERATION der Multiplikation
         *
         * (u64)num[i]:
         *   Cast zu u64 VOR der Multiplikation! Das ist entscheidend.
         *   Würden wir num[i] * factor als u32-Operation rechnen,
         *   würde das Produkt bereits als 32-Bit-Zahl berechnet und
         *   überlaufen, bevor der Cast angewendet wird.
         *   Mit dem Cast ZUERST wird die Multiplikation als 64-Bit-
         *   Operation durchgeführt.
         *
         * + carry:
         *   Der Übertrag vom vorherigen Block wird direkt addiert.
         *   Beim ersten Durchlauf (i=0) ist carry = 0.
         *
         * Beispiel: num[0] = 0xFFFFFFFF (= 4.294.967.295), factor = 2
         *   product = (u64)0xFFFFFFFF * 2 + 0 = 8.589.934.590 = 0x1FFFFFFFE
         * -------------------------------------------------------- */
        u64 product = (u64)num[i] * factor + carry;

        /* --------------------------------------------------------
         * Untere 32 Bit des Produkts → bleiben in diesem Block
         *
         * 0xFFFFFFFFUL ist eine Bitmaske: binär 32 Einsen.
         * product & 0xFFFFFFFF isoliert die unteren 32 Bit:
         *   product  = 0x1FFFFFFFE
         *   & mask   = 0x0FFFFFFFE  ← untere 32 Bit = 0xFFFFFFFE
         *
         * (u32)(...) Cast: Das Ergebnis der & Operation ist u64,
         * wir wollen es in den u32-Block schreiben → Cast sicher,
         * weil wir durch die Maske sichergestellt haben, dass nur
         * 32 Bit übrig sind.
         * -------------------------------------------------------- */
        num[i] = (u32)(product & 0xFFFFFFFFUL);

        /* --------------------------------------------------------
         * Obere 32 Bit des Produkts → werden zum carry für den nächsten Block
         *
         * >> 32: Bit-Shift nach rechts um 32 Stellen.
         * Alle unteren 32 Bit fallen weg, die oberen 32 rücken vor.
         *   product  = 0x1FFFFFFFE (64 Bit)
         *   >> 32    = 0x000000001 → carry = 1
         *
         * Beim nächsten Schleifendurchlauf (i+1) wird dieser carry
         * zum Produkt addiert.
         * -------------------------------------------------------- */
        carry = product >> 32;
    }

    /* ----------------------------------------------------------
     * Nach der Schleife: Ist noch Carry übrig?
     * ----------------------------------------------------------
     * Wenn nach dem höchsten Block noch ein Übertrag existiert,
     * muss die Zahl um einen neuen Block wachsen.
     *
     * (*size)++:
     *   - *size liest den aktuellen Wert von word_count (z.B. 5)
     *   - Das ++ NACH dem Ausdruck bedeutet: erst den Wert verwenden,
     *     dann erhöhen.
     *   - num[(*size)++] = carry schreibt carry in num[5]
     *     und erhöht word_count anschließend auf 6.
     *   - Wäre es ++(*size), würde zuerst erhöht und dann als Index
     *     genutzt – das wäre ein Off-by-One-Fehler!
     * ---------------------------------------------------------- */
    if (carry > 0) {
        num[(*size)++] = (u32)carry;
    }
}


/* ==================================================================
 * elapsed_ms() – Zeitdifferenz in Millisekunden berechnen
 * ==================================================================
 *
 * PARAMETER:
 *   struct timespec start  → Startzeitstempel (by value, nicht Zeiger)
 *   struct timespec end    → Endzeitstempel
 *
 * RÜCKGABE: double (64-Bit-Fließkommazahl) in Millisekunden
 *
 * RECHNUNG:
 *   Sekunden-Differenz:     (end.tv_sec - start.tv_sec) * 1000.0
 *     Sekunden in ms umrechnen: × 1000
 *     "1000.0" (mit Punkt) erzwingt Fließkommarechnung,
 *     sonst wäre es Integer-Division (würde Nachkommastellen verlieren).
 *
 *   Nanosekunden-Differenz: (end.tv_nsec - start.tv_nsec) / 1e6
 *     Nanosekunden → Millisekunden: ÷ 1.000.000
 *     "1e6" = 1 × 10^6 = 1.000.000 als double-Literal
 *     Die Division durch einen double macht das Ergebnis automatisch double.
 *
 *   HINWEIS: tv_nsec kann negativ werden wenn z.B.
 *     start.tv_nsec = 900.000.000 und end.tv_nsec = 100.000.000
 *     (weil eine Sekunde "übergelaufen" ist).
 *     Das ist korrekt, weil die Sekunden-Differenz dann 1 mehr ist
 *     und sich die negativen Nanosekunden damit ausgleichen.
 * ================================================================== */
double elapsed_ms(struct timespec start, struct timespec end) {
    return (end.tv_sec  - start.tv_sec)  * 1000.0   /* Sekunden → ms    */
         + (end.tv_nsec - start.tv_nsec) / 1e6;      /* Nanosekunden → ms */
}