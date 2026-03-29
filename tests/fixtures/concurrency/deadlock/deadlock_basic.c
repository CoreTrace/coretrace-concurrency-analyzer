// SPDX-License-Identifier: Apache-2.0
// Test 4: Deadlock - acquisition de locks dans ordre inverse
#include <pthread.h>

pthread_mutex_t lockA = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lockB = PTHREAD_MUTEX_INITIALIZER;

int shared_data1 = 0;
int shared_data2 = 0;

void* thread1_func(void* arg) {
    pthread_mutex_lock(&lockA);   // Lock A en premier
    // Simuler un peu de travail
    for (volatile int i = 0; i < 1000; i++);
    pthread_mutex_lock(&lockB);   // Puis lock B
    
    shared_data1++;
    shared_data2++;
    
    pthread_mutex_unlock(&lockB);
    pthread_mutex_unlock(&lockA);
    return NULL;
}

void* thread2_func(void* arg) {
    pthread_mutex_lock(&lockB);   // Lock B en premier (ORDRE INVERSE!)
    // Simuler un peu de travail
    for (volatile int i = 0; i < 1000; i++);
    pthread_mutex_lock(&lockA);   // Puis lock A
    
    shared_data1--;
    shared_data2--;
    
    pthread_mutex_unlock(&lockA);
    pthread_mutex_unlock(&lockB);
    return NULL;
}

int main() {
    pthread_t t1, t2;
    
    pthread_create(&t1, NULL, thread1_func, NULL);
    pthread_create(&t2, NULL, thread2_func, NULL);
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    
    return 0;
}
