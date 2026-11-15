// SPDX-License-Identifier: Apache-2.0
// first is handed a pointer a condition picks between the object's value and its handle, and
// writes a handle through it: where the pointer lies is not known, so it may land on the handle and
// finish may wait for another thread (#201).
// Expected: two data races, first against second and main against first.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
    pthread_t spare;
};

static int shared;

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void* second(void* argument)
{
    (void)argument;
    shared += 2;
    return NULL;
}

static void* first(void* argument)
{
    pthread_t* slot = argument;
    *slot = 0;
    shared += 1;
    return NULL;
}

static void finish(struct Task* task)
{
    pthread_join(task->thread, NULL);
}

int main(int argc, char** argv)
{
    (void)argv;
    struct Task task = {0};
    pthread_t later;
    pthread_create(&task.spare, NULL, idle, NULL);
    pthread_create(&task.thread, NULL, first, argc > 1 ? (void*)&task.thread : (void*)&task.value);
    finish(&task);
    pthread_create(&later, NULL, second, NULL);
    pthread_join(later, NULL);
    pthread_join(task.spare, NULL);
    return shared;
}
