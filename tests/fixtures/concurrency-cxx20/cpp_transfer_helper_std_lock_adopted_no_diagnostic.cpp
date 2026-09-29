// SPDX-License-Identifier: Apache-2.0
// The transfer helper takes both accounts' mutexes with std::lock, which never deadlocks whatever
// order it is given them, then hands them to guards built with std::adopt_lock. The two threads
// transfer in opposite directions.
// Expected: no diagnostic.
#include <mutex>
#include <thread>

struct Account
{
    std::mutex lock;
    int balance = 100;
};

static Account alice;
static Account bob;

static void transfer(Account& from, Account& to, int amount)
{
    std::lock(from.lock, to.lock);
    std::lock_guard<std::mutex> first(from.lock, std::adopt_lock);
    std::lock_guard<std::mutex> second(to.lock, std::adopt_lock);
    from.balance -= amount;
    to.balance += amount;
}

static void payAlice()
{
    transfer(bob, alice, 10);
}

int main()
{
    std::thread payer(payAlice);
    transfer(alice, bob, 10);
    payer.join();
    return alice.balance + bob.balance;
}
