// SPDX-License-Identifier: Apache-2.0
// The spawn loop runs twice (outer rounds) before any join, so two threads write each results[i]
// (#108).
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
    pthread_t threads[2][4];
    for (int round = 0; round < 2; ++round)
        for (int i = 0; i < 4; ++i)
            pthread_create(&threads[round][i], NULL, worker, &results[i]);
    for (int round = 0; round < 2; ++round)
        for (int i = 0; i < 4; ++i)
            pthread_join(threads[round][i], NULL);
    return results[0];
}
