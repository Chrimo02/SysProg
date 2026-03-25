/*
 * Fakultät – iterativ vs. rekursiv
 *
 * Berechnet n! auf zwei Arten und vergleicht deren Laufzeit.
 *
 * Ressourcenverbrauch im Vergleich:
 *
 * Laufzeit:
 *   Beide Varianten haben O(n) Zeitkomplexität. Rekursiv ist in der Praxis
 *   etwas langsamer, da jeder Aufruf einen neuen Stack-Frame anlegt
 *   (Sprung, Register sichern, Rückgabe).
 *
 * Speicher:
 *   Iterativ: O(1) – konstanter Speicher, nur eine Schleifenvariable.
 *   Rekursiv: O(n) – pro Aufruf wächst der Call-Stack um einen Frame.
 *   Bei sehr großem n kann das zu einem Stack-Overflow führen.
 * 
 * ##############
 * 
 * Ergebnisse von tests (Auf Laptop, nicht ESP32):
 * 
 * Natürliche Zahl eingeben: 3
 * 3! (iterativ)  = 6  [9.590 ms über 1000000 Iterationen]
 * 3! (rekursiv)  = 6  [8.630 ms über 1000000 Iterationen]
 * Rekursiv ist 1.11x schneller als iterativ.
 * 
 * Natürliche Zahl eingeben: 20
 * 20! (iterativ)  = 2432902008176640000  [41.101 ms über 1000000 Iterationen]
 * 20! (rekursiv)  = 2432902008176640000  [55.421 ms über 1000000 Iterationen]
 * Rekursiv ist 1.35x langsamer als iterativ.
 * 
 * Natürliche Zahl eingeben: 900
 * 900! (iterativ)  = 0  [808.061 ms über 1000000 Iterationen]
 * 900! (rekursiv)  = 0  [5730.972 ms über 1000000 Iterationen]
 * Rekursiv ist 7.09x langsamer als iterativ.
 * 
 * Natürliche Zahl eingeben: 10000
 * 10000! (iterativ)  = 0  [8642.812 ms über 1000000 Iterationen]
 * 10000! (rekursiv)  = 0  [67574.580 ms über 1000000 Iterationen]
 * Rekursiv ist 7.82x langsamer als iterativ.
 * 
 * ###############
 */

#include <stdio.h>
#include <time.h>

#define ITERATIONS 1000000

// Funktionsdeklarationen
long fakultaet_iterativ(int n);
long fakultaet_rekursiv(int n);
double elapsed_ms(struct timespec start, struct timespec end);

int main() {
    int n;
    printf("Natürliche Zahl eingeben: ");
    // %d ignoriert Kommastellen – bei "3.7" wird n=3 gelesen
    scanf("%d", &n);

    if (n < 0) {
        printf("Fehler: Fakultät nur für natürliche Zahlen definiert.\n");
        return 1;
    }

    struct timespec start, end;
    long result;

    // Iterativ messen
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        result = fakultaet_iterativ(n);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double time_iter = elapsed_ms(start, end);

    printf("%d! (iterativ)  = %ld  [%.3f ms über %d Iterationen]\n",
           n, result, time_iter, ITERATIONS);

    // Rekursiv messen
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        result = fakultaet_rekursiv(n);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double time_rec = elapsed_ms(start, end);

    printf("%d! (rekursiv)  = %ld  [%.3f ms über %d Iterationen]\n",
           n, result, time_rec, ITERATIONS);

    printf("\nRekursiv ist %.2fx %s als iterativ.\n",
           time_rec > time_iter ? time_rec / time_iter : time_iter / time_rec,
           time_rec > time_iter ? "langsamer" : "schneller");

    return 0;
}

// a) Iterative Variante
long fakultaet_iterativ(int n) {
    long result = 1;
    for (int i = 2; i <= n; i++) {
        result *= i;
    }
    return result;
}

// b) Rekursive Variante
long fakultaet_rekursiv(int n) {
    if (n <= 1) return 1;
    return n * fakultaet_rekursiv(n - 1);
}

double elapsed_ms(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000.0
         + (end.tv_nsec - start.tv_nsec) / 1e6;
}
