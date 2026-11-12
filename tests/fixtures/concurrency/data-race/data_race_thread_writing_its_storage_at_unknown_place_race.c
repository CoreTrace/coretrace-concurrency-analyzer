// SPDX-License-Identifier: Apache-2.0
// first writes the object it is handed at a place a variable picks, which may be its own handle:
// finish may wait for idle, and first runs beside second and past main's return (#201).
// Expected: data races of first against second and of main against first, and the object escaping
// main's frame.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
    pthread_t spare;
};

static int shared;
static size_t place = sizeof(int);

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
    unsigned char* bytes = argument;
    bytes[place] = 0;
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
