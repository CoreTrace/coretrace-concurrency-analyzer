// SPDX-License-Identifier: Apache-2.0
// The task keeps its thread handles in an array, its first field, and runs its thread on the
// second one. It is started twice and joined once, so the first thread is never joined and the
// read of `value` at the end races with it. A pointer to a handle may reach the whole array it
// belongs to, which is that first field and not the whole task: the second start does not race
// with the thread's write of `value` (#110).
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_t threads[2];
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
    pthread_create(&task->threads[1], NULL, run, task);
}

static void stop(struct Task* task)
{
    pthread_join(task->threads[1], NULL);
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
