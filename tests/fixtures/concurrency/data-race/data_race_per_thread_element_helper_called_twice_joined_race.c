// SPDX-License-Identifier: Apache-2.0
// start() hands &results[i] to thread i and returns without joining; main calls it twice, so two
// threads write each element (#108).
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
static void start(pthread_t* threads)
{
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
}
int main(void)
{
    pthread_t first[4];
    pthread_t second[4];
    start(first);
    start(second);
    for (int i = 0; i < 4; ++i)
    {
        pthread_join(first[i], NULL);
        pthread_join(second[i], NULL);
    }
    return results[0];
}
