// SPDX-License-Identifier: Apache-2.0
// writer takes the lock of the local object it is handed, which holds its handle; finish joins it
// through that object: the lock's bytes leave the handle unchanged, so the join waits for writer,
// before main reads shared and returns (#201).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static int shared;

static void* writer(void* argument)
{
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock);
    shared += 1;
    pthread_mutex_unlock(&held->lock);
    return NULL;
}

static void finish(struct Task* task)
{
    pthread_join(task->thread, NULL);
}

int main(void)
{
    struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
    pthread_create(&task.thread, NULL, writer, &task);
    finish(&task);
    return shared;
}
