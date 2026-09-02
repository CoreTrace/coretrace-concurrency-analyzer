// SPDX-License-Identifier: Apache-2.0
#include "worker.hpp"

Worker::Worker() : _running(true), _processed(0), _thread(&Worker::loop, this)
{
}

Worker::~Worker()
{
    stop();
    if (_thread.joinable())
        _thread.join();
}

void Worker::loop()
{
    while (_running)
    {
        UniqueLock held(_guard);

        // No predicate and no recheck: any wake-up is read as work having arrived.
        if (_idle.wait_for(held, std::chrono::milliseconds(10)) == std::cv_status::no_timeout)
            ++_processed;
    }
}

void Worker::stop()
{
    // Written from the caller's thread while `loop` reads it, with no lock on either side.
    _running = false;
    _idle.notify_all();
}
