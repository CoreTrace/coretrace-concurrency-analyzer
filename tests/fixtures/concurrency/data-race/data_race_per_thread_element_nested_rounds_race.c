// SPDX-License-Identifier: Apache-2.0
// The spawn loop runs twice (outer rounds) before any join, so two threads write each results[i]
// (#108).
// Nothing joins the threads: their join loop would not be matched (#173).
// Expected: one data race and one missing join.
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
    return 0;
}
