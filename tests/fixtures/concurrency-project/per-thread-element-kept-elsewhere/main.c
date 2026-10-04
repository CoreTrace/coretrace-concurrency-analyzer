// SPDX-License-Identifier: Apache-2.0
// Before the spawn loop, main hands &results[0] to remember(), defined in keeper.c, which
// publishes it; the observer thread there writes through it while thread 0 writes results[0]. No
// thread runs at the call, so only the element's identity can show the race (#108).
// Expected in project mode: one data race.
#include <pthread.h>
int results[4];
void remember(int* element);
void start_observer(void);
void join_observer(void);
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    start_observer();
    remember(&results[0]);
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    join_observer();
    return 0;
}
