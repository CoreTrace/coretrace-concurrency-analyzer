// SPDX-License-Identifier: Apache-2.0
// main increments slots[argc], and nothing calls main: no call supplies argc, so the element
// stays unknown there. The thread increments slots[1], which argc may name (#159).
// Expected: one data race on `slots`, main's increment against the thread's.
#include <pthread.h>
#include <stddef.h>

static int slots[4];

static void* worker(void* argument)
{
    (void)argument;
    slots[1] += 1;
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    slots[argc] += 1;
    pthread_join(thread, NULL);
    return slots[1];
}
