// SPDX-License-Identifier: Apache-2.0
// The element is picked by slot, a variable the loop never changes, not by the loop's counter:
// every thread is handed results[0] (#108).
// Expected: one data race.
#include <pthread.h>

static int results[4];

static void* worker(void* argument)
{
    int* element = argument;
    *element = 42;
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    int slot = 0;
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[slot]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return 0;
}
