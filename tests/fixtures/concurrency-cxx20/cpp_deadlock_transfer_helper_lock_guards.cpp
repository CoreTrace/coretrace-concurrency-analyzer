// SPDX-License-Identifier: Apache-2.0
// The bank transfer with std::lock_guard: the helper guards the payer's account, then the payee's.
// main pays bob from alice while the other thread pays alice from bob.
// Expected: one deadlock.
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
    std::lock_guard<std::mutex> first(from.lock);
    std::lock_guard<std::mutex> second(to.lock);
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
