// SPDX-License-Identifier: Apache-2.0
// writer tries the std::shared_mutex of the object it is handed, then takes it shared; the object
// also holds its pthread handle, which none of these calls changes, so main's join waits for writer
// (#201).
// Expected: no data race.
#include <pthread.h>
#include <shared_mutex>

struct Task
{
    std::shared_mutex lock;
    pthread_t thread;
};

static Task task;
static int shared;

static void* writer(void* argument)
{
    auto* held = static_cast<Task*>(argument);
    if (held->lock.try_lock())
    {
        shared += 1;
        held->lock.unlock();
    }
    held->lock.lock_shared();
    int seen = shared;
    held->lock.unlock_shared();
    return reinterpret_cast<void*>(static_cast<long>(seen));
}

int main()
{
    pthread_create(&task.thread, nullptr, writer, &task);
    pthread_join(task.thread, nullptr);
    return shared;
}
