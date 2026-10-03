// SPDX-License-Identifier: Apache-2.0
// Each worker writes 8 bytes through its 4-byte element, reaching into the next thread's element
// (#108).
// Expected: one data race.
#include <pthread.h>
static int results[6];
static void* worker(void* argument)
{
    long long* wide = argument;
    *wide = 0;
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
