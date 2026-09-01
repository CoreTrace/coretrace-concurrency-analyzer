// SPDX-License-Identifier: Apache-2.0
// The wait sits in a helper that forwards to the bare overload. A helper cannot recheck a
// condition it does not know, so the missing loop belongs to its caller — and that is where the
// diagnostic must land, not in the helper.
#include <chrono>
#include <condition_variable>
#include <mutex>

class Waiter
{
  public:
    std::cv_status wait_for(std::unique_lock<std::mutex>& held,
                            std::chrono::milliseconds timeout)
    {
        return _cv.wait_for(held, timeout);
    }

    void notify_all()
    {
        _cv.notify_all();
    }

  private:
    std::condition_variable _cv;
};

std::mutex state_lock;
Waiter waiter;
bool ready = false;

bool consume_once()
{
    std::unique_lock<std::mutex> held(state_lock);
    return waiter.wait_for(held, std::chrono::milliseconds(10)) == std::cv_status::no_timeout;
}

// The same helper, used correctly: the caller rechecks, so nothing is reported here.
void consume_until_ready()
{
    std::unique_lock<std::mutex> held(state_lock);
    while (!ready)
        (void)waiter.wait_for(held, std::chrono::milliseconds(10));
}
