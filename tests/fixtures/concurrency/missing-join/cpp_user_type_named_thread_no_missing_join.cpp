// SPDX-License-Identifier: Apache-2.0
// Regression: a user type whose mangled constructor contains "threadC1" is not a std::thread.
// Substring matching without a namespace guard reported every instance as an unjoined handle.
struct worker_thread
{
    explicit worker_thread(int identifier) : identifier_(identifier) {}

    int identifier_;
};

int main()
{
    worker_thread first(1);
    worker_thread second(2);
    return first.identifier_ + second.identifier_;
}
