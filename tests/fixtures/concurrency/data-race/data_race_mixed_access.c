// SPDX-License-Identifier: Apache-2.0
// Test 2: Data race - accès mixte lecture/écriture
#include <pthread.h>
#include <stdbool.h>

int shared_data = 0;
bool ready = false;  // Flag non protégé

void* writer(void* arg) {
    shared_data = 42;  // DATA RACE: écriture
    ready = true;      // DATA RACE: écriture de flag
    return NULL;
}

void* reader(void* arg) {
    while (!ready) {  // DATA RACE: lecture de flag
        // busy wait
    }
    int value = shared_data;  // DATA RACE: lecture
    printf("Value: %d\n", value);
    return NULL;
}

int main() {
    pthread_t t1, t2;
    
    pthread_create(&t1, NULL, writer, NULL);
    pthread_create(&t2, NULL, reader, NULL);
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    
    return 0;
}
