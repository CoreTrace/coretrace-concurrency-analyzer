// SPDX-License-Identifier: Apache-2.0
// writer locks the std::mutex of the object it is handed, which also holds its pthread handle: the
// mutex's bytes are not the handle's, so main's join waits for writer (#201).
// Expected: no data race.
#include <mutex>
#include <pthread.h>

struct Task
{
    std::mutex lock;
    pthread_t thread;
};

static Task task;
static int shared;

static void* writer(void* argument)
{
    auto* held = static_cast<Task*>(argument);
    held->lock.lock();
    shared += 1;
    held->lock.unlock();
    return nullptr;
}

int main()
{
    pthread_create(&task.thread, nullptr, writer, &task);
    pthread_join(task.thread, nullptr);
    return shared;
}
