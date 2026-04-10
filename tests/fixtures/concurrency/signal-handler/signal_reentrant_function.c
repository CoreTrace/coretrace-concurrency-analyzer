// SPDX-License-Identifier: Apache-2.0
// Test: Signal handler - fonction non réentrante appelée depuis un handler
//
// strtok() maintient un état interne statique (pointeur de contexte sauvegardé).
// Si un signal interrompt un appel à strtok() en cours et que le handler
// appelle strtok() lui-même, l'état interne est écrasé — le parsing
// interrompu dans le thread principal reprend avec un contexte corrompu.
// Correction : utiliser strtok_r() (réentrant) dans le handler et dans main.
#include <signal.h>
#include <stdio.h>
#include <string.h>

void handler(int sig) {
    char tmp[] = "X:Y:Z";
    // SIGNAL HANDLER: strtok() n'est pas réentrant.
    // Cet appel écrase le contexte interne de strtok() en cours dans main.
    char* tok = strtok(tmp, ":");
    while (tok) {
        tok = strtok(NULL, ":");
    }
    // À la sortie du handler, le contexte interne de strtok est celui de "X:Y:Z",
    // pas celui du parsing en cours dans main.
}

int main() {
    signal(SIGUSR1, handler);

    char input[] = "alpha:beta:gamma:delta";
    printf("Parsing: %s\n", input);

    char* tok = strtok(input, ":");
    while (tok != NULL) {
        printf("token: %s\n", tok);
        // SIGNAL HANDLER: si SIGUSR1 arrive ici, handler corrompt le contexte strtok
        raise(SIGUSR1);
        // SIGNAL HANDLER: strtok(NULL, ":") reprend depuis le contexte écrasé par le handler
        tok = strtok(NULL, ":");  // Résultat imprévisible
    }
    return 0;
}
