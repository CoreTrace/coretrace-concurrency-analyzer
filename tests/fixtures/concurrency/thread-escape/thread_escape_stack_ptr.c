// SPDX-License-Identifier: Apache-2.0
// Test: Thread escape - pointeur vers variable locale passé à un thread qui survit à la fonction
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

pthread_t global_thread;

void* use_pointer(void* arg) {
    usleep(10000);  // Simule un délai - la frame de pile peut être invalide
    int* ptr = (int*)arg;
    printf("Valeur: %d\n", *ptr);  // THREAD ESCAPE: ptr peut pointer vers une frame invalide
    return NULL;
}

void spawn_with_local() {
    int local_value = 99;  // Variable sur la pile
    pthread_create(&global_thread, NULL, use_pointer, &local_value);
    // THREAD ESCAPE: retour ici détruit local_value, mais le thread l'utilise encore
}

int main() {
    spawn_with_local();
    pthread_join(global_thread, NULL);
    return 0;
}
