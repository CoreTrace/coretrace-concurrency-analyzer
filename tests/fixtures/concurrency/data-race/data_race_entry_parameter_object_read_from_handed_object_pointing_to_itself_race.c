// SPDX-License-Identifier: Apache-2.0
// As data_race_object_read_from_handed_object_pointing_to_itself_race.c, the helper handed its
// entry as a parameter (#242).
// Expected: one data race, main's read of task against writer.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    struct Task* self;
    pthread_t thread;
};

static void* writer(void* argument)
{
    struct Task* task = argument;
    task->value += 1;
    return NULL;
}

static void start(struct Task* task, void* (*entry)(void*))
{
    pthread_create(&task->thread, NULL, entry, task->self);
}

int main(void)
{
    struct Task task = {0};
    struct Task other = {0};
    task.self = &task;
    start(&task, writer);
    int seen = task.value;
    int racing = other.value;
    pthread_join(task.thread, NULL);
    return seen + racing;
}
