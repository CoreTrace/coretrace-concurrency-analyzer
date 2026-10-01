// SPDX-License-Identifier: Apache-2.0
// main calls worker() itself only after joining the thread running it: the join orders the
// thread's write of `shared` before the call's (#155).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared = 42;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_join(thread, NULL);
    worker(NULL);
    return shared;
}
