// SPDX-License-Identifier: Apache-2.0
// Test: Missing join - seul le dernier handle de thread est conservé dans une boucle
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define N 10

int results[N];

void* compute(void* arg) {
    int id = *(int*)arg;
    results[id] = id * id;
    return NULL;
}

int main() {
    pthread_t t;  // Un seul handle réutilisé - les handles précédents sont perdus
    int ids[N];
    for (int i = 0; i < N; i++) {
        ids[i] = i;
        pthread_create(&t, NULL, compute, &ids[i]);  // MISSING JOIN: handle précédent écrasé
    }
    pthread_join(t, NULL);  // Joint seulement le dernier thread, les autres sont abandonnés
    for (int i = 0; i < N; i++) {
        printf("results[%d] = %d\n", i, results[i]);
    }
    return 0;
}
