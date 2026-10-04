// SPDX-License-Identifier: Apache-2.0
// The spawn loop hands each thread its own element of results, but extra.c starts the same
// worker on &results[0] while loop thread 0 writes it: the element is not that thread's own
// (#108).
// Expected in project mode: one data race.
#include <pthread.h>
int results[4];
void start_extra(void);
void join_extra(void);
void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    start_extra();
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    join_extra();
    return results[0];
}
