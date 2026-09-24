// SPDX-License-Identifier: Apache-2.0
// Test: Use-after-free - un thread libère la mémoire pendant qu'un autre l'utilise encore
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

typedef struct {
    int id;
    int value;
} Task;

Task* shared_task = NULL;

void* task_processor(void* arg) {
    usleep(5000);  // Simule un délai de traitement
    if (shared_task != NULL) {
        // USE-AFTER-FREE: shared_task peut avoir été libéré par task_owner
        printf("Traitement tâche id=%d value=%d\n",
               shared_task->id, shared_task->value);
    }
    return NULL;
}

void* task_owner(void* arg) {
    usleep(1000);
    free(shared_task);   // USE-AFTER-FREE: libéré pendant que task_processor l'utilise
    shared_task = NULL;
    printf("Tâche libérée\n");
    return NULL;
}

int main() {
    shared_task = malloc(sizeof(Task));
    shared_task->id    = 7;
    shared_task->value = 42;

    pthread_t t1, t2;
    pthread_create(&t1, NULL, task_processor, NULL);
    pthread_create(&t2, NULL, task_owner, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}
