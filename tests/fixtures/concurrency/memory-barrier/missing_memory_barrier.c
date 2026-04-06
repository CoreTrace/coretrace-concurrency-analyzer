// SPDX-License-Identifier: Apache-2.0
// Test 5: Missing memory barrier - visibilité des écritures
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

int data = 0;
bool flag = false;  // Devrait être atomique ou protégé par mémoire barrier

void* producer(void* arg) {
    data = 42;        // Écriture 1
    flag = true;      // Écriture 2 - peut être réordonnée avec data!
    return NULL;
}

void* consumer(void* arg) {
    while (!flag) {   // Lecture de flag
        // busy wait
    }
    // Sans mémoire barrier, data peut encore être 0 ici
    // même si flag est true (réordering CPU/compiler)
    int value = data;
    printf("Value: %d\n", value);
    return NULL;
}

int main() {
    pthread_t t1, t2;

    pthread_create(&t1, NULL, producer, NULL);
    pthread_create(&t2, NULL, consumer, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    return 0;
}
