// SPDX-License-Identifier: Apache-2.0
// One entry is started directly on a, and through a helper handed it as a parameter on b: main
// reads a while the thread on a writes it. The direct spawn keeps its object, so this race stays
// reported (#235). The thread on b is still taken to write a, an attribution kept from before the
// helper's spawn was settled: its race with the thread on a is false and is not pinned (#239).
// Expected: at least one data race, main against write_value.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
};

static void* write_value(void* argument)
{
    struct Task* task = argument;
    task->value += 1;
    return NULL;
}

static void start(struct Task* task, void* (*entry)(void*))
{
    pthread_create(&task->thread, NULL, entry, task);
}

int main(void)
{
    struct Task a = {0};
    struct Task b = {0};
    pthread_create(&a.thread, NULL, write_value, &a);
    start(&b, write_value);
    int result = a.value;
    pthread_join(a.thread, NULL);
    pthread_join(b.thread, NULL);
    return result;
}
