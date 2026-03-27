// Test 6: Violation d'ordre de locks - potentiel deadlock
#include <pthread.h>

pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex2 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex3 = PTHREAD_MUTEX_INITIALIZER;

void* worker_a(void* arg) {
    pthread_mutex_lock(&mutex1);
    pthread_mutex_lock(&mutex2);  // Ordre: 1 -> 2
    
    // Section critique
    pthread_mutex_unlock(&mutex2);
    pthread_mutex_unlock(&mutex1);
    return NULL;
}

void* worker_b(void* arg) {
    pthread_mutex_lock(&mutex2);
    pthread_mutex_lock(&mutex3);  // Ordre: 2 -> 3
    
    // Section critique
    pthread_mutex_unlock(&mutex3);
    pthread_mutex_unlock(&mutex2);
    return NULL;
}

void* worker_c(void* arg) {
    pthread_mutex_lock(&mutex3);
    pthread_mutex_lock(&mutex1);  // Ordre: 3 -> 1 (CRÉCYCLE!)
    
    // Section critique
    pthread_mutex_unlock(&mutex1);
    pthread_mutex_unlock(&mutex3);
    return NULL;
}

int main() {
    pthread_t t1, t2, t3;
    
    pthread_create(&t1, NULL, worker_a, NULL);
    pthread_create(&t2, NULL, worker_b, NULL);
    pthread_create(&t3, NULL, worker_c, NULL);
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    
    return 0;
}
