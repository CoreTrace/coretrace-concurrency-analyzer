// SPDX-License-Identifier: Apache-2.0
// The loop breaks right after starting thread 2; main then writes results[i], the element thread 2
// may still be writing (#108).
// Expected: one data race. Every thread is joined, but the completion proof does not match this
// join (#173): the false missing join it reports is left unchecked.
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
    int i = 0;
    while (i < 4)
    {
        pthread_create(&threads[i], NULL, worker, &results[i]);
        if (i == 2)
            break;
        i++;
    }
    results[i] = -1;
    for (int k = 0; k <= i; ++k)
        pthread_join(threads[k], NULL);
    return 0;
}
