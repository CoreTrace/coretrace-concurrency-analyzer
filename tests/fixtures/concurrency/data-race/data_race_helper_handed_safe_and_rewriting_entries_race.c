// SPDX-License-Identifier: Apache-2.0
// One helper starts and joins the entry it is handed, called once with an entry leaving the handle
// alone and once with one replacing it by a thread still idle: the join of the second call may
// wait for that one, so rewriting may still run when main reads second. The safe entry settled at
// the other call does not make the helper's join end it (#201, #235). main does not read first,
// whose thread the helper's join is no longer taken to end either (#240).
// Expected: at least one data race, main against rewriting.
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

static void* safe(void* argument)
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

static void runToEnd(struct Task* task, void* (*entry)(void*))
{
    pthread_create(&task->thread, NULL, entry, task);
    pthread_join(task->thread, NULL);
}

int main(void)
{
    struct Task first = {0};
    struct Task second = {0};
    pthread_create(&second.spare, NULL, idle, NULL);
    runToEnd(&first, safe);
    runToEnd(&second, rewriting);
    int seen = second.value;
    pthread_join(second.thread, NULL);
    return seen;
}
