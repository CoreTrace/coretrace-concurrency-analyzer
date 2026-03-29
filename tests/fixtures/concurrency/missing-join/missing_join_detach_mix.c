// SPDX-License-Identifier: Apache-2.0
// Test: Missing join avec détachement incorrect
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

int shared_counter = 0;

void* incrementer(void* arg) {
    for (int i = 0; i < 1000; i++) {
        shared_counter++;
    }
    printf("Incrementer done, counter = %d\n", shared_counter);
    return NULL;
}

void* checker(void* arg) {
    sleep(1);  // Attend un peu
    printf("Checker sees counter = %d\n", shared_counter);
    return NULL;
}

int main() {
    pthread_t t1, t2;
    
    pthread_create(&t1, NULL, incrementer, NULL);
    pthread_create(&t2, NULL, checker, NULL);
    
    // Détache le premier thread (pas de join possible après)
    pthread_detach(t1);
    
    // MISSING JOIN: t2 n'est ni joint ni détaché explicitement
    // Le thread peut encore être en exécution à la fin du main
    
    printf("Main exiting, counter = %d\n", shared_counter);
    return 0;
}
