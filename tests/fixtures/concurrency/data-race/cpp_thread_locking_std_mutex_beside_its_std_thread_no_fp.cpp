// SPDX-License-Identifier: Apache-2.0
// writer locks the std::mutex of the object holding its std::thread, which main moves the started
// thread into and joins there: locking leaves the thread member alone (#201).
// Expected: no data race.
#include <mutex>
#include <thread>

struct Task
{
    std::mutex lock;
    std::thread thread;
};

static Task task;
static int shared;

static void writer(Task* held)
{
    held->lock.lock();
    shared += 1;
    held->lock.unlock();
}

int main()
{
    task.thread = std::thread(writer, &task);
    task.thread.join();
    return shared;
}
