// Test 3: Race condition - check-then-use (TOCTOU)
#include <pthread.h>
#include <stdlib.h>

int* shared_ptr = NULL;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void* allocator(void* arg) {
    if (shared_ptr == NULL) {  // CHECK
        shared_ptr = malloc(100);  // USE: race avec d'autres threads
        *shared_ptr = 42;
    }
    return NULL;
}

void* user(void* arg) {
    if (shared_ptr != NULL) {  // CHECK
        int value = *shared_ptr;  // USE: peut être NULL entre temps
        printf("Value: %d\n", value);
    }
    return NULL;
}

int main() {
    pthread_t threads[4];
    
    for (int i = 0; i < 2; i++) {
        pthread_create(&threads[i], NULL, allocator, NULL);
    }
    for (int i = 2; i < 4; i++) {
        pthread_create(&threads[i], NULL, user, NULL);
    }
    
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    
    if (shared_ptr) free(shared_ptr);
    return 0;
}
