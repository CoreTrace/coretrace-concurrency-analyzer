// SPDX-License-Identifier: Apache-2.0
// first waits on the condition variable of the global object it is handed, which also holds its
// handle, and the predicate the wait runs replaces that handle through the global. A wait that runs
// a predicate is no lock operation: it may change more than the condition variable, so finish may
// wait for idle and first runs beside second and past main's return (#201).
// Expected: three data races, the predicate's write of the handle against finish's read, first
// against second, and main against first.
#include <condition_variable>
#include <mutex>
#include <pthread.h>

struct Task
{
    std::condition_variable ready;
    pthread_t thread;
    pthread_t spare;
};

static Task task;
static std::mutex guard;
static int shared;

static void* idle(void*)
{
    return nullptr;
}

static void* second(void*)
{
    shared += 2;
    return nullptr;
}

static void* first(void* argument)
{
    auto* held = static_cast<Task*>(argument);
    const auto replaceHandle = []
    {
        task.thread = task.spare;
        return true;
    };
    std::unique_lock<std::mutex> lock(guard);
    held->ready.wait(lock, replaceHandle);
    lock.unlock();
    shared += 1;
    return nullptr;
}

static void finish(Task* waited)
{
    pthread_join(waited->thread, nullptr);
}

int main()
{
    pthread_t later;
    pthread_create(&task.spare, nullptr, idle, nullptr);
    pthread_create(&task.thread, nullptr, first, &task);
    finish(&task);
    pthread_create(&later, nullptr, second, nullptr);
    pthread_join(later, nullptr);
    pthread_join(task.spare, nullptr);
    return shared;
}
