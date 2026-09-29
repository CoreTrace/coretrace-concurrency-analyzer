// SPDX-License-Identifier: Apache-2.0
// Each round starts a worker and, from the second round on, tests the status the previous round's
// join kept before reading `shared`: that test says nothing about this round's worker, which may
// still be writing `shared` while main reads it. Nothing checks the last join, and a failed join
// lets a worker run into the next round.
// Expected: two data races on `shared`, main's read against a worker's write, and worker against
// worker.
#include <pthread.h>
#include <stdlib.h>
static int shared;
static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
int main(void)
{
    pthread_t thread;
    int status;
    int total = 0;
    for (int round = 0; round < 2; ++round)
    {
        pthread_create(&thread, NULL, worker, NULL);
        if (round > 0)
        {
            if (status != 0)
                abort();
            total += shared;
        }
        status = pthread_join(thread, NULL);
    }
    return total;
}
