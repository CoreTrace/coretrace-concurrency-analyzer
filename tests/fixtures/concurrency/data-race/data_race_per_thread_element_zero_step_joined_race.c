// SPDX-License-Identifier: Apache-2.0
// The counter's step is zero; the loop stops after two spawns, both handed &results[0] (#108).
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
    pthread_t threads[2];
    int started = 0;
    for (int i = 0; i < 4; i += 0)
    {
        pthread_create(&threads[started], NULL, worker, &results[i]);
        if (++started == 2)
            break;
    }
    for (int k = 0; k < started; ++k)
        pthread_join(threads[k], NULL);
    return results[0];
}
