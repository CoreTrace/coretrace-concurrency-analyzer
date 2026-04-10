// SPDX-License-Identifier: Apache-2.0
// Test: Signal handler - appel de pthread_mutex_lock depuis un handler (deadlock)
//
// pthread_mutex_lock n'est pas dans la liste des fonctions async-signal-safe de POSIX.
// Si le signal est livré au thread pendant qu'il détient déjà le mutex,
// le handler tente de le verrouiller à nouveau sur un mutex non récursif :
// deadlock garanti. Même sans réacquisition du même verrou, appeler
// pthread_mutex_lock depuis un handler est un comportement indéfini (POSIX).
// Correction : utiliser uniquement des fonctions async-signal-safe dans les handlers
// (write(), sem_post(), les opérations sur sig_atomic_t).
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t counter = 0;

void handler(int sig) {
    // SIGNAL HANDLER: pthread_mutex_lock n'est pas async-signal-safe.
    // Si le signal interrompt la section critique ci-dessous pendant que
    // le thread tient mtx, cette tentative de verrouillage provoque un deadlock.
    pthread_mutex_lock(&mtx);    // NON ASYNC-SIGNAL-SAFE: deadlock si déjà tenu
    counter++;
    pthread_mutex_unlock(&mtx);
}

int main() {
    signal(SIGUSR1, handler);

    for (int i = 0; i < 20; i++) {
        pthread_mutex_lock(&mtx);
        // SIGNAL HANDLER: si SIGUSR1 arrive entre ce lock et le unlock,
        // le handler tente pthread_mutex_lock sur un mutex déjà tenu -> deadlock
        kill(getpid(), SIGUSR1);
        pthread_mutex_unlock(&mtx);
    }

    printf("counter = %d\n", (int)counter);
    return 0;
}
