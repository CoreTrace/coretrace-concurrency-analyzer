// SPDX-License-Identifier: Apache-2.0
// Test 14: Race condition fork/thread - état inconsistant après fork
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int shared_counter = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void* incrementer(void* arg) {
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&lock);
        shared_counter++;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int main() {
    pthread_t t;
    pthread_create(&t, NULL, incrementer, NULL);
    
    // Fork pendant que l'autre thread travaille!
    pid_t pid = fork();
    
    if (pid == 0) {
        // Processus enfant - seul le thread appelant fork() survit
        // Mais shared_counter peut être dans un état inconsistant!
        printf("Child: counter = %d\n", shared_counter);
        
        // Le mutex peut être locked dans le parent mort!
        pthread_mutex_lock(&lock);  // POTENTIEL DEADLOCK
        shared_counter += 1000;
        pthread_mutex_unlock(&lock);
        
        printf("Child: final counter = %d\n", shared_counter);
        exit(0);
    } else {
        pthread_join(t, NULL);
        printf("Parent: counter = %d\n", shared_counter);
    }
    
    return 0;
}
