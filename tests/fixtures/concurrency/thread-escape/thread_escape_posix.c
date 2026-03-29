// SPDX-License-Identifier: Apache-2.0
// Test 11: Thread escape - fonction appelée parfois avec/sans thread
#include <pthread.h>
#include <stdio.h>

int shared_buffer[100];
int buffer_index = 0;  // Non protégé

// Cette fonction est appelée depuis un thread ET depuis main
void add_to_buffer(int value) {
    if (buffer_index < 100) {
        shared_buffer[buffer_index] = value;  // DATA RACE potentiel
        buffer_index++;
    }
}

void* thread_func(void* arg) {
    int id = *(int*)arg;
    for (int i = 0; i < 10; i++) {
        add_to_buffer(id * 10 + i);  // Appel depuis thread
    }
    return NULL;
}

int main() {
    pthread_t t;
    int thread_id = 1;
    
    pthread_create(&t, NULL, thread_func, &thread_id);
    
    // Main appelle aussi la fonction sans synchronisation!
    for (int i = 0; i < 10; i++) {
        add_to_buffer(i);  // Appel depuis main thread - DATA RACE!
    }
    
    pthread_join(t, NULL);
    
    printf("Buffer filled up to index: %d\n", buffer_index);
    return 0;
}
