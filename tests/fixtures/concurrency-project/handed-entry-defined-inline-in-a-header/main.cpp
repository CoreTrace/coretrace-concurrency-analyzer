// SPDX-License-Identifier: Apache-2.0
// Unit of the spawn: writer, defined inline in worker.hpp, increments shared under the lock of the
// task it is handed. main hands writer and its own task to a helper, which starts writer as a
// thread on task, and starts reader on task, which reads shared under task's lock. Once the thread
// is joined, main calls run_other, which another unit defines, while reader runs. A unit using
// writer defines its own copy of it and declares none: only that copy tells this unit's analysis
// that writer may run on another object (#235).
#include "worker.hpp"

#include <pthread.h>

int shared;

void run_other();

static void* reader(void* argument)
{
    Task* held = static_cast<Task*>(argument);
    pthread_mutex_lock(&held->lock);
    int value = shared;
    pthread_mutex_unlock(&held->lock);
    return reinterpret_cast<void*>(static_cast<long>(value));
}

static void start(Task* started, void* (*entry)(void*))
{
    pthread_create(&started->thread, nullptr, entry, started);
}

int main()
{
    Task task = {PTHREAD_MUTEX_INITIALIZER, {}};
    pthread_t back;
    start(&task, writer);
    pthread_create(&back, nullptr, reader, &task);
    pthread_join(task.thread, nullptr);
    run_other();
    pthread_join(back, nullptr);
    return shared;
}
