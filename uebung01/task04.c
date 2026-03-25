/*
 * Periodisches Signal + Datum/Uhrzeit im Sekundentakt
 *
 * a. Alle 5 Sekunden wird ein akustisches Signal (BEL, '\a') ausgegeben.
 * b. Jede Sekunde wird das aktuelle Datum und die Uhrzeit ausgegeben.
 *
 * Beide Aufgaben laufen in derselben Hauptschleife – der 5-Sekunden-Takt
 * ergibt sich durch Modulo auf den Sekundenzähler.
 *
 * Zeitmessung: time() liefert die Systemzeit als time_t (Sekunden seit
 * dem Unix-Epoch 1970-01-01 00:00:00 UTC).
 * Formatierung: strftime() aus <time.h> wandelt struct tm in lesbaren Text.
 *
 * Schleife beenden: Strg+C
 */

#include <stdio.h>
#include <time.h>      /* time(), localtime(), strftime(), difftime() */

#define SIGNAL_INTERVAL_SEC 5   /* Alle N Sekunden einen Ton */

int main(void) {
    printf("Datum/Uhrzeit-Ausgabe gestartet. Beenden mit Strg+C.\n");
    printf("Ton alle %d Sekunden.\n\n", SIGNAL_INTERVAL_SEC);

    int tick = 0;               /* Sekundenzähler für Modulo-Vergleich  */
    time_t last = time(NULL);   /* Zeitstempel des letzten Ticks        */

    while (1) {
        /* Warte, bis eine volle Sekunde vergangen ist.
           difftime() liefert die Differenz in Sekunden als double –
           damit bleibt der Code portabel, auch wenn time_t kein int ist. */
        time_t now;
        do {
            now = time(NULL);
        } while (difftime(now, last) < 1.0);

        last = now;             /* Referenzzeit für den nächsten Tick  */
        tick++;

        /* b. Datum und Uhrzeit formatiert ausgeben */
        struct tm *t = localtime(&now);  /* UTC → lokale Zeitzone       */
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d  %H:%M:%S", t);
        printf("[%s]", buf);

        /* a. Periodisches Signal alle SIGNAL_INTERVAL_SEC Sekunden */
        if (tick % SIGNAL_INTERVAL_SEC == 0) {
            printf("  \a*** SIGNAL (alle %ds) ***", SIGNAL_INTERVAL_SEC);
            fflush(stdout);     /* '\a' (BEL) sofort an Terminal senden */
        }

        printf("\n");
    }

    return 0;   /* Nicht erreichbar, aber guter Stil */
}
