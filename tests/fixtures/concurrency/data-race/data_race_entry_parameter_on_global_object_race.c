// SPDX-License-Identifier: Apache-2.0
// The helper starts the entry it is handed on a global it names itself: main reads the global
// while the thread writes it through its argument (#235).
// Expected: one data race, main against other.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
};

static struct Task shared;

static void* other(void* argument)
{
    struct Task* task = argument;
    task->value += 2;
    return NULL;
}

static void start(void* (*entry)(void*))
{
    pthread_create(&shared.thread, NULL, entry, &shared);
}

int main(void)
{
    start(other);
    int seen = shared.value;
    pthread_join(shared.thread, NULL);
    return seen;
}
