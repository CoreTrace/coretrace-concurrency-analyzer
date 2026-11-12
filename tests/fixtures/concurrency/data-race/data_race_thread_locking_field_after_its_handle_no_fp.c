// SPDX-License-Identifier: Apache-2.0
// writer takes a lock declared after its handle in the object it is handed: the lock sits at a
// nonzero offset, past the handle's bytes, and main's join still waits for writer (#201). The
// handle is not declared first, where the lowering names the global itself.
// Expected: no data race.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
    pthread_mutex_t lock;
};

static struct Task task = {0, 0, PTHREAD_MUTEX_INITIALIZER};
static int shared;

static void* writer(void* argument)
{
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock);
    shared += 1;
    pthread_mutex_unlock(&held->lock);
    return NULL;
}

int main(void)
{
    pthread_create(&task.thread, NULL, writer, &task);
    pthread_join(task.thread, NULL);
    return shared;
}
