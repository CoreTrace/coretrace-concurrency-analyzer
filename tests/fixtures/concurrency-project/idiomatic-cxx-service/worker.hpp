// SPDX-License-Identifier: Apache-2.0
#ifndef IDIOMATIC_CXX_SERVICE_WORKER_HPP_
#define IDIOMATIC_CXX_SERVICE_WORKER_HPP_

#include "sync.hpp"

#include <thread>

/// Shared state held as members, reached through `this`, and a thread started from a
/// pointer-to-member: the ordinary shape of a C++ service.
class Worker
{
  public:
    Worker();
    ~Worker();

    void stop();

  private:
    void loop();

    Mutex _guard;
    ConditionVariable _idle;
    bool _running;
    int _processed;
    std::thread _thread;
};

#endif
