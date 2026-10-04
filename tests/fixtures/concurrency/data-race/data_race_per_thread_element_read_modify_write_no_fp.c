// SPDX-License-Identifier: Apache-2.0
// Each thread reads and writes only its own element (*slot += 1); main writes every element before
// the spawn loop and reads after the join loop (#108).
// Expected: no diagnostic.
#include <pthread.h>
static int counters[4];
static void* worker(void* argument)
{
    int* slot = argument;
    for (int n = 0; n < 1000; ++n)
        *slot += 1;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
        counters[i] = 0;
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &counters[i]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return counters[1];
}
