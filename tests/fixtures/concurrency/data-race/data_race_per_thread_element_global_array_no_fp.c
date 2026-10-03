// SPDX-License-Identifier: Apache-2.0
// Each thread writes only its own element of the global results; main reads after the join loop
// (#108's reproducer, global array) (#108).
// Expected: no diagnostic.
#include <pthread.h>
static int results[4];
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return results[0];
}
