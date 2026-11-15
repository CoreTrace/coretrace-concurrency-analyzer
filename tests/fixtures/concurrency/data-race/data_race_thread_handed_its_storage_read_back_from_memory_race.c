// SPDX-License-Identifier: Apache-2.0
// The object holding the handle keeps its own address, which main reads back and hands to
// rewriting: rewriting may replace the handle, so main's join may wait for idle while rewriting
// still writes shared (#201). The race on the handle itself is not reported: the object rewriting
// is handed is not known.
// Expected: one data race, main against rewriting.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
    pthread_t spare;
    struct Task* self;
};

static int shared;

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void* rewriting(void* argument)
{
    struct Task* task = argument;
    task->thread = task->spare;
    shared += 1;
    return NULL;
}

int main(void)
{
    struct Task task = {0};
    task.self = &task;
    pthread_create(&task.spare, NULL, idle, NULL);
    pthread_create(&task.thread, NULL, rewriting, task.self);
    pthread_join(task.thread, NULL);
    int seen = shared;
    pthread_join(task.spare, NULL);
    return seen;
}
