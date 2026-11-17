// SPDX-License-Identifier: Apache-2.0
// A callee starts a thread on another handle before joining the one main started, joins both, then
// starts a third on main's handle: the first two run together, the third beside neither (#201).
// Expected: one data race, run against itself.
#include <pthread.h>
#include <stddef.h>

static int shared;
static pthread_t spare;

struct Task
{
    pthread_t thread;
};

static void* run(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void restart(struct Task* task)
{
    pthread_create(&spare, NULL, run, NULL);
    pthread_join(task->thread, NULL);
    pthread_join(spare, NULL);
    pthread_create(&task->thread, NULL, run, NULL);
}

int main(void)
{
    struct Task task;
    pthread_create(&task.thread, NULL, run, NULL);
    restart(&task);
    pthread_join(task.thread, NULL);
    return shared;
}
