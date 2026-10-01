// SPDX-License-Identifier: Apache-2.0
// main reads through the published address before it starts the thread, then starts and joins it:
// no thread of this unit has published anything yet at that read.
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static _Thread_local int slot;
static int* published;

static void* worker(void* argument)
{
    (void)argument;
    slot = 1;
    published = &slot;
    return NULL;
}

int main(void)
{
    int before = published != NULL ? *published : 0;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_join(thread, NULL);
    return before;
}
