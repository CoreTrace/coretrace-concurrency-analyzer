// SPDX-License-Identifier: Apache-2.0
// Every safe C++ spelling of a wait, in one unit. None may be reported.
//
// A captureless lambda is an empty class, and clang drops it from the lowered signature: both
// overloads then arrive with the same number of arguments. What separates them is the return
// type the standard mandates, `cv_status` against `bool`, which the mangled name keeps.
#include <chrono>
#include <condition_variable>
#include <mutex>

std::mutex state_lock;
std::condition_variable state_changed;
bool ready = false;

void wait_with_predicate()
{
    std::unique_lock<std::mutex> held(state_lock);
    state_changed.wait(held, [] { return ready; });
}

void wait_for_with_predicate()
{
    std::unique_lock<std::mutex> held(state_lock);
    state_changed.wait_for(held, std::chrono::milliseconds(10), [] { return ready; });
}

void wait_until_with_predicate()
{
    std::unique_lock<std::mutex> held(state_lock);
    state_changed.wait_until(held, std::chrono::steady_clock::now() + std::chrono::seconds(1),
                             [] { return ready; });
}

// The bare overload is safe too, as long as the caller rechecks.
void bare_wait_inside_a_loop()
{
    std::unique_lock<std::mutex> held(state_lock);
    while (!ready)
        state_changed.wait(held);
}
