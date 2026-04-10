// SPDX-License-Identifier: Apache-2.0
// Test: Use-after-free - work item libéré avant que le thread worker l'exécute
//
// Un struct de travail contenant un pointeur de callback et des données est soumis
// à un thread worker. L'appelant libère le struct immédiatement après la soumission,
// sans aucune synchronisation. Le worker déréférence le pointeur de callback et
// les données du struct libéré : comportement indéfini garanti.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    void (*on_complete)(int result);
    int   payload;
    char  tag[32];
} WorkItem;

void handle_result(int result) {
    printf("Result: %d\n", result);
}

void* worker(void* arg) {
    WorkItem* item = (WorkItem*)arg;
    usleep(5000);  // Délai — l'appelant a le temps de libérer item

    // USE-AFTER-FREE: item->on_complete et item->payload accédés après free()
    item->on_complete(item->payload * 2);
    return NULL;
}

int main() {
    WorkItem* item = malloc(sizeof(WorkItem));
    item->on_complete = handle_result;
    item->payload     = 21;

    pthread_t t;
    pthread_create(&t, NULL, worker, item);

    // USE-AFTER-FREE: libère item sans attendre que le worker ait terminé
    free(item);
    item = NULL;

    pthread_join(t, NULL);
    return 0;
}
