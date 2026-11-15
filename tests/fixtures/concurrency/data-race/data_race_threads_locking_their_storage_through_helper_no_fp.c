// SPDX-License-Identifier: Apache-2.0
// Two threads of writer share the object holding both their handles and take its lock through a
// helper: the lock's bytes change under the runtime's synchronization, which is no data access, and
// neither handle is touched, so both joins wait for their thread (#201).
// Expected: no data race.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
    pthread_t other;
};

static struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0, 0};
static int shared;

static void lock_task(struct Task* locked)
{
    pthread_mutex_lock(&locked->lock);
}

static void unlock_task(struct Task* locked)
{
    pthread_mutex_unlock(&locked->lock);
}

static void* writer(void* argument)
{
    struct Task* held = argument;
    lock_task(held);
    shared += 1;
    unlock_task(held);
    return NULL;
}

int main(void)
{
    pthread_create(&task.thread, NULL, writer, &task);
    pthread_create(&task.other, NULL, writer, &task);
    pthread_join(task.thread, NULL);
    pthread_join(task.other, NULL);
    return shared;
}
