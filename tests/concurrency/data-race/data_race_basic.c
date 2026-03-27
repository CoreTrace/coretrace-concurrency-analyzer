// Test 1: Data race basique - écriture concurrente sans synchronisation
#include <pthread.h>
#include <stdio.h>

int shared_counter = 0;  // Variable partagée non protégée

void* increment(void* arg) {
    for (int i = 0; i < 10000; i++) {
        shared_counter++;  // DATA RACE: lecture-modification-écriture non atomique
    }
    return NULL;
}

int main() {
    pthread_t t1, t2;
    
    pthread_create(&t1, NULL, increment, NULL);
    pthread_create(&t2, NULL, increment, NULL);
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    
    printf("Counter: %d (attendu: 20000)\n", shared_counter);
    return 0;
}
