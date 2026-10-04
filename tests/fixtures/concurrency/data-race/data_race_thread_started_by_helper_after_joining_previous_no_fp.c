// SPDX-License-Identifier: Apache-2.0
// The first thread is started before the loop; each round joins the thread the previous one
// started, going on only past the join's success, then starts the next through the helper into
// the same handle. No two threads run at once (#126).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void start(pthread_t* thread)
{
    pthread_create(thread, NULL, worker, NULL);
}

int main(void)
{
    pthread_t thread;
    start(&thread);
    for (int i = 1; i < 3; ++i)
    {
        if (pthread_join(thread, NULL) != 0)
            return 1;
        start(&thread);
    }
    pthread_join(thread, NULL);
    return 0;
}
