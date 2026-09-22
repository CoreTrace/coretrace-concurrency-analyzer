// SPDX-License-Identifier: Apache-2.0
// Test: Signal handler - masque de signaux non configuré dans les threads workers
//
// Dans un programme multi-thread, un signal peut être livré à n'importe quel
// thread qui ne le masque pas. Sans appel à pthread_sigmask() dans les workers,
// un signal destiné au thread principal peut être intercepté par un worker.
// Le handler s'exécute alors dans le contexte du worker et modifie des données
// "main-only" sans synchronisation — data race entre le handler et main.
// Correction : masquer les signaux dans les workers avec pthread_sigmask(SIG_BLOCK).
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static int  admin_state  = 0;  // Censé n'être modifié que par le thread principal
static volatile sig_atomic_t shutdown_flag = 0;

void handler(int sig) {
    // SIGNAL HANDLER: peut s'exécuter dans un worker (qui n'a pas masqué SIGALRM).
    // admin_state est lu/écrit sans synchronisation avec main.
    admin_state  = -1;   // DATA RACE: écrit depuis un worker via le handler
    shutdown_flag = 1;
}

void* worker(void* arg) {
    // SIGNAL HANDLER: les workers ne masquent pas les signaux.
    // pthread_sigmask(SIG_BLOCK, &sigset, NULL) manque ici.
    while (!shutdown_flag) {
        usleep(500);
        (void)admin_state;  // DATA RACE: lu ici, potentiellement écrit par le handler
                            // dans ce même thread
    }
    return NULL;
}

int main() {
    signal(SIGALRM, handler);

    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, worker, NULL);
    }

    admin_state = 42;
    ualarm(10000, 0);  // SIGALRM livré à un thread aléatoire non masqué

    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("admin_state = %d\n", admin_state);
    return 0;
}
