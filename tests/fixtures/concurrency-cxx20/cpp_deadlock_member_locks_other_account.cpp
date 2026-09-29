// SPDX-License-Identifier: Apache-2.0
// A member function locks its own account's mutex, then the other account's. alice pays bob while
// bob pays alice, so each thread can hold the mutex the other waits for.
// Expected: one deadlock.
#include <mutex>
#include <thread>

class Account
{
  public:
    void transferTo(Account& other, int amount)
    {
        std::lock_guard<std::mutex> mine(lock_);
        std::lock_guard<std::mutex> theirs(other.lock_);
        balance_ -= amount;
        other.balance_ += amount;
    }

    int balance() const
    {
        return balance_;
    }

  private:
    std::mutex lock_;
    int balance_ = 100;
};

static Account alice;
static Account bob;

static void payAlice()
{
    bob.transferTo(alice, 10);
}

int main()
{
    std::thread payer(payAlice);
    alice.transferTo(bob, 10);
    payer.join();
    return alice.balance() + bob.balance();
}
