// SPDX-License-Identifier: Apache-2.0
// Round k writes results[k] and then starts a thread on results[k + 1]; results[k] belongs to the
// thread round k - 1 started, which may still run (#108).
// Expected: one data race. Every thread is joined, but the completion proof does not match this
// join (#173): the false missing join it reports is left unchecked.
#include <pthread.h>
static int results[5];
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
        results[i] = -1;
        i++;
        pthread_create(&threads[i - 1], NULL, worker, &results[i]);
    }
    for (int k = 0; k < 4; ++k)
        pthread_join(threads[k], NULL);
    return 0;
}
