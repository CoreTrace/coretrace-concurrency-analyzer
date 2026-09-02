// SPDX-License-Identifier: Apache-2.0
// A wake-up proves nothing: `notify_all` wakes every waiter, and the standard permits a wake-up
// with no notify at all. Both functions below read "I woke" as "the condition holds".
#include <chrono>
#include <condition_variable>
#include <mutex>

std::mutex state_lock;
std::condition_variable state_changed;
bool ready = false;

void wait_without_rechecking()
{
    std::unique_lock<std::mutex> held(state_lock);
    state_changed.wait(held);
}

// The timed form is worse than it looks: a spurious wake-up returns `no_timeout`, which reads
// as "the deadline did not pass, so someone notified me".
void timed_wait_without_rechecking()
{
    std::unique_lock<std::mutex> held(state_lock);
    (void)state_changed.wait_for(held, std::chrono::milliseconds(10));
}
