// SPDX-License-Identifier: Apache-2.0
// Two different threads both call store(), which writes `shared`, but main joins `first` before
// it starts `second`: the join orders the two writes (#155).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void store(void)
{
    shared = 42;
}

static void* first(void* argument)
{
    (void)argument;
    store();
    return NULL;
}

static void* second(void* argument)
{
    (void)argument;
    store();
    return NULL;
}

int main(void)
{
    pthread_t firstThread;
    pthread_t secondThread;
    pthread_create(&firstThread, NULL, first, NULL);
    pthread_join(firstThread, NULL);
    pthread_create(&secondThread, NULL, second, NULL);
    pthread_join(secondThread, NULL);
    return shared;
}
