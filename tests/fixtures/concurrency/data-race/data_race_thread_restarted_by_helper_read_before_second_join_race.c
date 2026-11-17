// SPDX-License-Identifier: Apache-2.0
// A helper joins the thread main started, then starts another on the same handle: main reads while
// the second runs, before joining it. The helper's own join comes before the thread it leaves
// running, so it does not end it (#201).
// Expected: one data race, main against run. The thread main started ended at the helper's join:
// run does not race with itself.
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

static void restart(struct Task* task)
{
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, run, task);
}

int main(void)
{
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    restart(&task);
    int seen = task.value;
    pthread_join(task.thread, NULL);
    return seen;
}
