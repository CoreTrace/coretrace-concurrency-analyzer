// SPDX-License-Identifier: Apache-2.0
// Regression: std::recursive_mutex is designed to be relocked by its owner. Its mangled name
// matched the plain mutex heuristic, so the legal pattern was reported as a self-deadlock.
#include <mutex>
#include <thread>

std::recursive_mutex reentrant_lock;

void relock()
{
    reentrant_lock.lock();
    reentrant_lock.lock();
    reentrant_lock.unlock();
    reentrant_lock.unlock();
}

int main()
{
    std::thread worker(relock);
    worker.join();
    return 0;
}
