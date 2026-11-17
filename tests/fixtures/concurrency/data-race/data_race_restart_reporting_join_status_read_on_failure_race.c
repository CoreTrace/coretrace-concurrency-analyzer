// SPDX-License-Identifier: Apache-2.0
// A callee joins the thread main started and, only when that join succeeds, starts another into
// the handle; it returns the join's status. Main reads on the branch where the join failed: the
// first thread may still run there (#201).
// Expected: main's read on the failure branch races with run; the thread may outlive the object it
// is handed. The race of run with itself, false (#253), is not pinned.
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

static int restart(struct Task* task)
{
    int status = pthread_join(task->thread, NULL);
    if (status == 0)
        pthread_create(&task->thread, NULL, run, task);
    return status;
}

int main(void)
{
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    if (restart(&task) != 0)
        return task.value;
    pthread_join(task.thread, NULL);
    return task.value;
}
