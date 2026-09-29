// SPDX-License-Identifier: Apache-2.0
// The task holds its thread handle as its first field, and `start` hands it to `pthread_create`
// through a local pointer. It is started twice and joined once, so the first thread is never
// joined and the read of `value` at the end races with it. The local pointer still designates
// the first field: the second start writes the handle, not the whole task, and does not race with
// the thread's write of `value`; the join through the field names the handle the starts wrote,
// so the handle is not reported as never joined (#110).
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_t thread;
    int value;
};

static void* run(void* argument)
{
    struct Task* task = argument;
    task->value += 1;
    return NULL;
}

static void start(struct Task* task)
{
    pthread_t* handle = &task->thread;
    pthread_create(handle, NULL, run, task);
}

static void stop(struct Task* task)
{
    pthread_join(task->thread, NULL);
}

int main(int argc, char** argv)
{
    (void)argv;
    struct Task task = {0};
    (void)argc;
    start(&task);
    start(&task);
    stop(&task);
    return task.value;
}
