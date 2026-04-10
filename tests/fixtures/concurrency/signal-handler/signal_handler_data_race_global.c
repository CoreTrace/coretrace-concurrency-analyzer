// SPDX-License-Identifier: Apache-2.0
// Test: Signal handler - écriture dans une variable globale non volatile sig_atomic_t
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

// SIGNAL HANDLER: doit être volatile sig_atomic_t pour un accès sûr depuis un handler
int shutdown_requested = 0;  // Non volatile, non atomique
long work_done = 0;          // Non volatile, non atomique

void handle_sigint(int sig) {
    shutdown_requested = 1;  // SIGNAL HANDLER: écriture non atomique, peut être réordonnée
    // work_done lu ici peut être partiellement mis à jour (déchirement de valeur sur 32 bits)
    printf("Arrêt demandé après %ld unités de travail\n", work_done);
}

void* worker(void* arg) {
    while (!shutdown_requested) {  // DATA RACE: lecture pendant écriture dans le handler
        work_done++;               // DATA RACE: modifié ici et lu dans le handler
        usleep(100);
    }
    return NULL;
}

int main() {
    signal(SIGINT, handle_sigint);
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    sleep(1);
    raise(SIGINT);
    pthread_join(t, NULL);
    return 0;
}
