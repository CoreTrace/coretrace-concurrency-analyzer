// SPDX-License-Identifier: Apache-2.0
// main points the field at other, then stores moved's address at a place in task chosen at run
// time: it is the field, and writer runs on moved (#242). Where that write lands is not known,
// so what the field holds is not proven, and writer keeps task, which main does not read.
// Expected: no race of main's read of other with writer. The race of main's read of moved with
// writer is missed, and is not pinned.
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

static void start(struct Task* task)
{
    pthread_create(&task->thread, NULL, writer, task->self);
}

int main(void)
{
    volatile int field = 1;
    struct Task task = {0};
    struct Task other = {0};
    struct Task moved = {0};
    task.self = &other;
    ((struct Task**)&task)[field] = &moved;
    start(&task);
    int racing = moved.value;
    int quiet = other.value;
    pthread_join(task.thread, NULL);
    return racing + quiet;
}
