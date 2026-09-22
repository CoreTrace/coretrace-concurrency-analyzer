// SPDX-License-Identifier: Apache-2.0
// Test: Missing join - retour anticipé sur chemin d'erreur laisse un thread orphelin
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

void* background_task(void* arg) {
    for (volatile int i = 0; i < 1000000; i++);
    printf("Tâche de fond terminée\n");
    return NULL;
}

int do_work(int input) {
    pthread_t bg;
    pthread_create(&bg, NULL, background_task, NULL);

    if (input < 0) {
        // MISSING JOIN: retour anticipé sans joindre bg
        fprintf(stderr, "Erreur: entrée négative\n");
        return -1;
    }

    pthread_join(bg, NULL);
    return input * 2;
}

int main() {
    int result = do_work(-1);  // Déclenche le chemin d'erreur
    printf("Résultat: %d\n", result);
    return 0;
}
