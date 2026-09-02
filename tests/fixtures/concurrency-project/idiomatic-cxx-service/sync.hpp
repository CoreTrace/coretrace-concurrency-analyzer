// SPDX-License-Identifier: Apache-2.0
// The synchronisation wrappers a C++ codebase writes for itself. Nothing here is wrong; they
// exist because every rule has to keep working when the primitive is one layer down.
#ifndef IDIOMATIC_CXX_SERVICE_SYNC_HPP_
#define IDIOMATIC_CXX_SERVICE_SYNC_HPP_

#include <chrono>
#include <condition_variable>
#include <mutex>

class Mutex
{
  public:
    void lock()
    {
        _mutex.lock();
    }

    void unlock()
    {
        _mutex.unlock();
    }

    std::mutex& variable() noexcept
    {
        return _mutex;
    }

  private:
    std::mutex _mutex;
};

class UniqueLock
{
  public:
    explicit UniqueLock(Mutex& mutex) : _lock(mutex.variable())
    {
    }

    std::unique_lock<std::mutex>& variable() noexcept
    {
        return _lock;
    }

  private:
    std::unique_lock<std::mutex> _lock;
};

class ConditionVariable
{
  public:
    void notify_all()
    {
        _condition.notify_all();
    }

    std::cv_status wait_for(UniqueLock& held, std::chrono::milliseconds timeout)
    {
        return _condition.wait_for(held.variable(), timeout);
    }

  private:
    std::condition_variable _condition;
};

#endif
