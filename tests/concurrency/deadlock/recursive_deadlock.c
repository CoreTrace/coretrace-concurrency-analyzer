// Test 13: Deadlock récursif - thread s'attend lui-même
#include <pthread.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;  // Non récursif par défaut!

void helper_function() {
    pthread_mutex_lock(&lock);   // Deuxième acquisition du même lock
    // Fait quelque chose
    pthread_mutex_unlock(&lock);
}

void* worker(void* arg) {
    pthread_mutex_lock(&lock);   // Première acquisition
    
    // Appelle une fonction qui tente de réacquérir le même lock
    helper_function();  // DEADLOCK: le thread s'attend lui-même!
    
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main() {
    pthread_t t;
    
    pthread_create(&t, NULL, worker, NULL);
    pthread_join(t, NULL);
    
    return 0;
}
