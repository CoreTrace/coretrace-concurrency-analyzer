// SPDX-License-Identifier: Apache-2.0
// A helper joins the thread main started, then starts the entry it is handed on the same handle.
// That entry replaces the handle with a thread that is still idle, so main's join may wait for
// that one and not for it: main races with rewriting, never with run, which the helper joined
// (#201, #235).
// Expected: at least two data races, main against rewriting on the handle and on value, and one
// missing join; the race of rewriting with run is the one #233 tracks, and is not pinned.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
    pthread_t spare;
};

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void* run(void* argument)
{
    struct Task* task = argument;
    task->value += 1;
    return NULL;
}

static void* rewriting(void* argument)
{
    struct Task* task = argument;
    task->thread = task->spare;
    task->value += 1;
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
    pthread_create(&task.spare, NULL, idle, NULL);
    pthread_create(&task.thread, NULL, run, &task);
    restart(&task, rewriting);
    pthread_join(task.thread, NULL);
    return task.value;
}
