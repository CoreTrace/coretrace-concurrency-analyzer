// SPDX-License-Identifier: Apache-2.0
// Test: Signal handler - appel de fonction non async-signal-safe depuis un handler
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void signal_handler(int sig) {
    // SIGNAL HANDLER: printf, malloc, free ne sont pas async-signal-safe
    // Appeler ces fonctions depuis un handler peut causer deadlock ou corruption
    printf("Signal %d reçu\n", sig);       // NON ASYNC-SIGNAL-SAFE
    char* buf = malloc(64);                // NON ASYNC-SIGNAL-SAFE: peut deadlocker si
    if (buf) {                             // le signal interrompt un malloc en cours
        snprintf(buf, 64, "handled");
        free(buf);                         // NON ASYNC-SIGNAL-SAFE
    }
}

void* worker(void* arg) {
    for (int i = 0; i < 100000; i++) {
        void* p = malloc(16);  // Peut être interrompu par le signal -> deadlock interne malloc
        free(p);
    }
    return NULL;
}

int main() {
    signal(SIGALRM, signal_handler);

    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);

    ualarm(1000, 0);  // Déclenche SIGALRM pendant que worker fait des malloc

    pthread_join(t, NULL);
    return 0;
}
