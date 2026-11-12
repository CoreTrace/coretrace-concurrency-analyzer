// SPDX-License-Identifier: Apache-2.0
// first hands the object to a function this unit only declares, as a pointer to const: it may
// keep the address for anything to replace the handle through (#201).
// Expected: three data races, first against second, main against first, and main's join of spare
// against the call handing the storage on, which finish's join also conflicts with; and the object
// escaping main's frame.
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

void publish(const struct Task* task);

static void* first(void* argument)
{
    publish(argument);
    shared += 1;
    return NULL;
}

static void finish(struct Task* task)
{
    pthread_join(task->thread, NULL);
}

int main(void)
{
    struct Task task = {0};
    pthread_t later;
    pthread_create(&task.spare, NULL, idle, NULL);
    pthread_create(&task.thread, NULL, first, &task);
    finish(&task);
    pthread_create(&later, NULL, second, NULL);
    pthread_join(later, NULL);
    pthread_join(task.spare, NULL);
    return shared;
}
