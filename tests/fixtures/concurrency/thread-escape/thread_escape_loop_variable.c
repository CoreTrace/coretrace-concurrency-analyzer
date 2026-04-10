// SPDX-License-Identifier: Apache-2.0
// Test: Thread escape - adresse de variable de boucle partagée entre threads
#include <pthread.h>
#include <stdio.h>

#define N 5

void* print_id(void* arg) {
    int id = *(int*)arg;  // THREAD ESCAPE: i a peut-être déjà changé dans la boucle
    printf("Thread id: %d\n", id);
    return NULL;
}

int main() {
    pthread_t threads[N];
    for (int i = 0; i < N; i++) {
        pthread_create(&threads[i], NULL, print_id, &i);  // THREAD ESCAPE: &i réutilisé à chaque itération
    }
    for (int i = 0; i < N; i++) {
        pthread_join(threads[i], NULL);
    }
    return 0;
}
