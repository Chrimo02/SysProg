/*
 * Binärer Baum B(E, V) mit V = 65 Knoten, fortlaufend nummeriert (1..65)
 *
 * Darstellung: vollständiger Binärbaum als Array (Heap-Darstellung)
 *   Knoten i  →  linkes Kind:  2*i
 *                rechtes Kind: 2*i + 1
 *                Elter:        i / 2
 *   Index 0 bleibt ungenutzt, Knoten 1 ist die Wurzel.
 *
 *   Tiefenstruktur:
 *     Ebene 0:  1 Knoten  (Knoten   1)
 *     Ebene 1:  2 Knoten  (Knoten   2–3)
 *     Ebene 2:  4 Knoten  (Knoten   4–7)
 *     Ebene 3:  8 Knoten  (Knoten   8–15)
 *     Ebene 4: 16 Knoten  (Knoten  16–31)
 *     Ebene 5: 32 Knoten  (Knoten  32–63)
 *     Ebene 6:  2 Knoten  (Knoten  64–65, ganz links)
 *
 * a. Baumausgabe: Level-by-Level + seitlich gedreht (alle Ebenen 0–6)
 *
 * b. Traversierung:
 *    i.  Tiefensuche  (DFS) → Pre-Order  (Wurzel → Links → Rechts)
 *    ii. Breitensuche (BFS) → Level-Order (Ebene für Ebene, Queue-basiert)
 *
 * iv. Antwort:
 *    DFS verwendet hier PRE-ORDER.
 *      Pre-Order:  Wurzel zuerst, dann links, dann rechts.
 *      In-Order:   Links, dann Wurzel, dann rechts.
 *      Post-Order: Links, rechts, Wurzel zuletzt.
 *    BFS/Level-Order ist keine der drei DFS-Formen – sie arbeitet
 *    ebenenweise und benötigt eine Queue statt Rekursion.
 */

#include <stdio.h>

#define N          65    /* Anzahl Knoten                        */
#define SHOW_MAX   N     /* Seitliche Darstellung aller Knoten   */
#define QUEUE_CAP  128   /* Queue-Kapazität für BFS (muss > N)   */

/* Vorwärtsdeklarationen */
static void print_levels(void);
static void print_sideways(int node, int depth, int limit);
static void dfs_preorder(int node, int *count);
static void bfs_levelorder(void);

/* =========================================================================
 * main
 * ========================================================================= */
int main(void) {
    printf("=== Binärer Baum B(E,V) mit V = %d Knoten ===\n\n", N);

    /* ------------------------------------------------------------------ */
    /* a. Baumdarstellung                                                  */
    /* ------------------------------------------------------------------ */
    printf("--- a. Ebenen-Uebersicht ---\n");
    print_levels();

    printf("\n--- a. Seitliche Darstellung (alle Ebenen 0-6, Knoten 1..%d) ---\n",
           SHOW_MAX);
    printf("    (rechts im Terminal = oberer Ast, links = unterer Ast)\n\n");
    print_sideways(1, 0, SHOW_MAX);

    /* ------------------------------------------------------------------ */
    /* b.i  DFS – Pre-Order                                               */
    /* ------------------------------------------------------------------ */
    printf("\n--- b.i  Tiefensuche (DFS) - Pre-Order ---\n    ");
    int count = 0;
    dfs_preorder(1, &count);
    printf("\n");

    /* ------------------------------------------------------------------ */
    /* b.ii BFS – Level-Order                                             */
    /* ------------------------------------------------------------------ */
    printf("\n--- b.ii Breitensuche (BFS) - Level-Order ---\n    ");
    bfs_levelorder();
    printf("\n");

    /* ------------------------------------------------------------------ */
    /* b.iv Erklärung der verwendeten Traversierungsformen                */
    /* ------------------------------------------------------------------ */
    printf("\n--- b.iv Verwendete Traversierungsformen ---\n");
    printf("  DFS -> PRE-ORDER\n");
    printf("    Reihenfolge: Wurzel zuerst, dann linker, dann rechter Teilbaum.\n");
    printf("    Merkmal: Die Wurzel erscheint als erstes Element.\n\n");
    printf("  Nicht gewaehlte DFS-Formen (zum Vergleich):\n");
    printf("    IN-ORDER:   Links -> Wurzel -> Rechts\n");
    printf("                Liefert bei binaeren Suchbaeumen eine sortierte Folge.\n");
    printf("    POST-ORDER: Links -> Rechts -> Wurzel\n");
    printf("                Wurzel erscheint als letztes Element.\n\n");
    printf("  BFS -> LEVEL-ORDER  (keine DFS-Form)\n");
    printf("    Besucht alle Knoten ebenenweise von oben nach unten.\n");
    printf("    Benoetigt eine Queue; nicht rekursiv.\n");

    return 0;
}

/* =========================================================================
 * Ebenen-Übersicht: listet Knotennummern pro Ebene auf
 * ========================================================================= */
static void print_levels(void) {
    int level = 0;
    int start = 1;   /* erster Knoten der aktuellen Ebene */

    while (start <= N) {
        /* Letzter Knoten dieser Ebene (entweder 2*start-1 oder N) */
        int end = 2 * start - 1;
        if (end > N) end = N;

        printf("  Ebene %d (%2d Knoten): ", level, end - start + 1);
        for (int i = start; i <= end; i++) {
            printf("%d", i);
            if (i < end) printf(", ");
        }
        printf("\n");

        start = 2 * start;   /* erste Kind-Ebene beginnt hier */
        level++;
    }
}

/* =========================================================================
 * Seitliche Baumdarstellung (rekursiv, 90° gedreht)
 *
 * Rechter Teilbaum → aktueller Knoten → linker Teilbaum.
 * "Rechts" erscheint oben im Terminal, "links" unten.
 * limit: Knoten mit Nummer > limit werden nicht ausgegeben.
 * ========================================================================= */
static void print_sideways(int node, int depth, int limit) {
    if (node > limit) return;
    print_sideways(2 * node + 1, depth + 1, limit);   /* rechts */
    for (int i = 0; i < depth; i++) printf("    ");
    printf("%3d\n", node);
    print_sideways(2 * node,     depth + 1, limit);   /* links  */
}

/* =========================================================================
 * DFS – Pre-Order: Wurzel → linker Teilbaum → rechter Teilbaum
 * ========================================================================= */
static void dfs_preorder(int node, int *count) {
    if (node > N) return;

    printf("%3d ", node);
    if (++(*count) % 13 == 0) printf("\n    ");   /* Umbruch alle 13 Werte */

    dfs_preorder(2 * node,     count);   /* linker Teilbaum  */
    dfs_preorder(2 * node + 1, count);   /* rechter Teilbaum */
}

/* =========================================================================
 * BFS – Level-Order: Queue-basierte Breitensuche
 *
 * Alle Knoten einer Ebene werden besucht, bevor die nächste Ebene
 * beginnt. Dazu werden Kinder in die Queue eingereiht und der
 * Reihe nach abgearbeitet.
 * ========================================================================= */
static void bfs_levelorder(void) {
    int queue[QUEUE_CAP];
    int head = 0, tail = 0;
    int count = 0;

    queue[tail++] = 1;   /* Wurzel in die Queue einreihen */

    while (head < tail) {
        int node = queue[head++];   /* vorderstes Element entnehmen */

        printf("%3d ", node);
        if (++count % 13 == 0) printf("\n    ");

        /* Kinder einreihen, falls vorhanden */
        if (2 * node     <= N) queue[tail++] = 2 * node;
        if (2 * node + 1 <= N) queue[tail++] = 2 * node + 1;
    }
}
