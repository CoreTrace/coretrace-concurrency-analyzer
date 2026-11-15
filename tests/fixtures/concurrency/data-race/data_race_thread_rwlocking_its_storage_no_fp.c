// SPDX-License-Identifier: Apache-2.0
// writer takes the read-write lock of the object it is handed, for writing then for reading; the
// object also holds its handle, which the lock's bytes leave alone, so main's join waits for writer
// (#201).
// Expected: no data race.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_rwlock_t lock;
    pthread_t thread;
};

static struct Task task = {PTHREAD_RWLOCK_INITIALIZER, 0};
static int shared;

static void* writer(void* argument)
{
    struct Task* held = argument;
    pthread_rwlock_wrlock(&held->lock);
    shared += 1;
    pthread_rwlock_unlock(&held->lock);
    pthread_rwlock_rdlock(&held->lock);
    int seen = shared;
    pthread_rwlock_unlock(&held->lock);
    return (void*)(long)seen;
}

int main(void)
{
    pthread_create(&task.thread, NULL, writer, &task);
    pthread_join(task.thread, NULL);
    return shared;
}
