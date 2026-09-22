// SPDX-License-Identifier: Apache-2.0
// Test: Data race - accès concurrent à des bitfields distincts partageant la même unité de stockage
//
// Deux threads accèdent à des champs de bits DIFFÉRENTS d'un même struct.
// Bien qu'ils n'écrivent pas le même champ logique, le compilateur implémente
// la modification d'un bitfield par un read-modify-write de toute l'unité de
// stockage sous-jacente (ex: un int de 32 bits). L'accès concurrent constitue
// donc une data race, même si les bits ciblés sont disjoints.
// Correction : utiliser _Atomic ou protéger l'ensemble du struct avec un mutex.
#include <pthread.h>
#include <stdio.h>

struct StatusWord {
    unsigned int ready    : 1;
    unsigned int error    : 1;
    unsigned int priority : 4;
    unsigned int count    : 10;
};

static struct StatusWord status = {0, 0, 0, 0};

void* set_ready(void* arg) {
    for (int i = 0; i < 200000; i++) {
        // DATA RACE: le compilateur génère un load+mask+store de l'unité entière
        status.ready = (unsigned)(i & 1);
    }
    return NULL;
}

void* set_error(void* arg) {
    for (int i = 0; i < 200000; i++) {
        // DATA RACE: même read-modify-write de la même unité de stockage
        // en concurrent avec set_ready — les écritures peuvent s'écraser mutuellement
        status.error = (unsigned)(i & 1);
    }
    return NULL;
}

int main() {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, set_ready, NULL);
    pthread_create(&t2, NULL, set_error, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("ready=%u error=%u priority=%u count=%u\n",
           status.ready, status.error, status.priority, status.count);
    return 0;
}
