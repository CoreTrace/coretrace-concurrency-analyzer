// SPDX-License-Identifier: Apache-2.0
// An account locks its own mutex and, still holding it, calls a member of the other account that
// locks the other's. alice pays bob while bob pays alice.
// Expected: one deadlock.
#include <mutex>
#include <thread>

class Account
{
  public:
    void deposit(int amount)
    {
        std::lock_guard<std::mutex> guard(lock_);
        balance_ += amount;
    }

    void transferTo(Account& other, int amount)
    {
        std::lock_guard<std::mutex> guard(lock_);
        balance_ -= amount;
        other.deposit(amount);
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
