// SPDX-License-Identifier: Apache-2.0
// A helper joins the thread main started, then starts the entry it is handed on the same handle:
// main reads while that entry runs, and races with it, never with run, which the helper joined
// (#201, #235).
// Expected: at least one data race, main against other; the race of other with run is the one
// #233 tracks, and is not pinned.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
};

static void* run(void* argument)
{
    struct Task* task = argument;
    task->value += 1;
    return NULL;
}

static void* other(void* argument)
{
    struct Task* task = argument;
    task->value += 2;
    return NULL;
}

static void restart(struct Task* task, void* (*entry)(void*))
{
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, entry, task);
}

int main(void)
{
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    restart(&task, other);
    int seen = task.value;
    pthread_join(task.thread, NULL);
    return seen;
}
