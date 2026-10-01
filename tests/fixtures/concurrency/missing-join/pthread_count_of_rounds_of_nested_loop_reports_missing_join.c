// SPDX-License-Identifier: Apache-2.0
// started counts the rounds of an inner loop that runs once per round of an outer loop of unknown
// length, so it may stay 0: the loop joining threads[0..started) then joins neither thread.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t threads[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    int started = 0;
    for (int round = 0; round < argc - 1; round++)
        for (int i = 0; i < 2; i++)
            ++started;
    for (int k = 0; k < started; k++)
        pthread_join(threads[k], NULL);
    return 0;
}
