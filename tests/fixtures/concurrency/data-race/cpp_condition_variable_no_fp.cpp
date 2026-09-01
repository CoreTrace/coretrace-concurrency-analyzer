// SPDX-License-Identifier: Apache-2.0
// Regression: a textbook wait/notify pair. The reader's unique_lock was not recognized as
// holding the mutex, and the condition variable object was reported as racing data.
#include <condition_variable>
#include <mutex>
#include <thread>

std::mutex state_lock;
std::condition_variable state_changed;
bool ready = false;

void produce()
{
    {
        std::lock_guard<std::mutex> held(state_lock);
        ready = true;
    }
    state_changed.notify_one();
}

void consume()
{
    std::unique_lock<std::mutex> held(state_lock);
    state_changed.wait(held, [] { return ready; });
}

int main()
{
    std::thread producer(produce);
    std::thread consumer(consume);
    producer.join();
    consumer.join();
    return ready ? 0 : 1;
}
