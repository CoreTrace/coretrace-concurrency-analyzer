// SPDX-License-Identifier: Apache-2.0
// on shared: main's shared = round in round 1 against the round-0 reader. The seen slots are per
// thread; main reads them after every join (#108).
// Expected: one data race.
#include <pthread.h>
static int shared;
static int seen[2];
static void* reader(void* argument)
{
    int* slot = argument;
    *slot = shared;
    return NULL;
}
int main(void)
{
    pthread_t threads[2];
    for (int round = 0; round < 2; ++round)
    {
        shared = round;
        pthread_create(&threads[round], NULL, reader, &seen[round]);
    }
    for (int round = 0; round < 2; ++round)
        pthread_join(threads[round], NULL);
    return seen[0] + seen[1];
}
