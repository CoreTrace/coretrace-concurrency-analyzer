// SPDX-License-Identifier: Apache-2.0
// Each round starts a reader and joins it only in the next round, after main has written
// shared: the reader of the previous round may still run then (#113).
// Expected: one data race.
#include <pthread.h>
static int shared;
static void* reader(void* argument)
{
    (void)argument;
    return (void*)(long)shared;
}
int main(void)
{
    pthread_t threads[2];
    for (int round = 0; round < 2; ++round)
    {
        shared = round;
        pthread_create(&threads[round], NULL, reader, NULL);
    }
    for (int round = 0; round < 2; ++round)
        pthread_join(threads[round], NULL);
    return 0;
}
