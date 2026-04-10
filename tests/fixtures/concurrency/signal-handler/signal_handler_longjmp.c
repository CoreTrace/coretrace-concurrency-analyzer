// SPDX-License-Identifier: Apache-2.0
// Test: Signal handler - longjmp hors du handler corrompt la pile du thread
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static sigjmp_buf jump_buffer;

void handle_sigusr1(int sig) {
    // SIGNAL HANDLER: siglongjmp depuis un handler est extrêmement dangereux
    // Saute hors du handler sans restaurer le masque de signal correctement,
    // laisse des ressources non libérées, corrompt potentiellement la pile du thread
    siglongjmp(jump_buffer, 1);  // NON SÉCURISÉ: fuite de contexte signal, pile corrompue
}

void* risky_thread(void* arg) {
    if (sigsetjmp(jump_buffer, 1) == 0) {
        printf("Thread: avant le signal\n");
        sleep(1);
        printf("Thread: après sleep (ne devrait pas arriver)\n");
    } else {
        // SIGNAL HANDLER: retour ici via longjmp - état du thread indéfini
        printf("Thread: retour via longjmp depuis handler - état indéfini\n");
    }
    return NULL;
}

int main() {
    signal(SIGUSR1, handle_sigusr1);
    pthread_t t;
    pthread_create(&t, NULL, risky_thread, NULL);
    usleep(100000);
    pthread_kill(t, SIGUSR1);  // Envoie le signal au thread
    pthread_join(t, NULL);
    return 0;
}
