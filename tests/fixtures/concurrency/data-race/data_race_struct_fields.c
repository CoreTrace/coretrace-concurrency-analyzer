// SPDX-License-Identifier: Apache-2.0
// Test: Data race sur les champs d'une structure partagée
#include <pthread.h>
#include <stdio.h>

typedef struct {
    int x;
    int y;
    int z;
} Point;

Point shared_point = {0, 0, 0};  // Structure partagée non protégée

void* writer_a(void* arg) {
    for (int i = 0; i < 10000; i++) {
        shared_point.x = i;        // DATA RACE: écriture concurrente sur x
        shared_point.y = i * 2;    // DATA RACE: écriture concurrente sur y
    }
    return NULL;
}

void* writer_b(void* arg) {
    for (int i = 0; i < 10000; i++) {
        shared_point.x = -i;       // DATA RACE: écriture concurrente sur x
        shared_point.z = i + 1;    // DATA RACE: écriture concurrente sur z
    }
    return NULL;
}

int main() {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, writer_a, NULL);
    pthread_create(&t2, NULL, writer_b, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("Point: (%d, %d, %d)\n", shared_point.x, shared_point.y, shared_point.z);
    return 0;
}
