// SPDX-License-Identifier: Apache-2.0
// writer hands the guard nested in the object it is handed, past its handle, to helpers that lock
// the guard's own lock: the lock lies where the guard does in the object, not at the guard's offset
// zero, and main's join waits for writer (#201).
// Expected: no data race.
#include <pthread.h>
#include <stddef.h>

struct Guard
{
    pthread_mutex_t lock;
};

struct Task
{
    int value;
    pthread_t thread;
    struct Guard guard;
};

static struct Task task = {0, 0, {PTHREAD_MUTEX_INITIALIZER}};
static int shared;

static void enter(struct Guard* guard)
{
    pthread_mutex_lock(&guard->lock);
}

static void leave(struct Guard* guard)
{
    pthread_mutex_unlock(&guard->lock);
}

static void* writer(void* argument)
{
    struct Task* held = argument;
    enter(&held->guard);
    shared += 1;
    leave(&held->guard);
    return NULL;
}

int main(void)
{
    pthread_create(&task.thread, NULL, writer, &task);
    pthread_join(task.thread, NULL);
    return shared;
}
