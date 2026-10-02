// SPDX-License-Identifier: Apache-2.0
// main keeps the handle array's address in a local whose address it hands to a function, which
// overwrites threads[0] with threads[1]: the first thread is never joined, and runs when main
// writes.
// Expected: one data race and one missing join.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void overwrite_first(pthread_t** handles)
{
    (*handles)[0] = (*handles)[1];
}

int main(void)
{
    pthread_t threads[2];
    pthread_t* saved = threads;
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    overwrite_first(&saved);
    for (int i = 0; i < 2; i++)
        pthread_join(threads[i], NULL);
    shared += 2;
    return shared;
}
