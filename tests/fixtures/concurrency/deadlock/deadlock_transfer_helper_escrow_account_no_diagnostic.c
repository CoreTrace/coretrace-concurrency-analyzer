// SPDX-License-Identifier: Apache-2.0
// main pays bob through an escrow account private to the call: it locks alice, then the escrow,
// and later the escrow, then bob. The worker locks bob, then alice. main never holds alice and bob
// together. The escrow is the helper's lock at each call, not the caller's parameter in that
// position.
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Account
{
    pthread_mutex_t lock;
    int balance;
};

static struct Account alice = {PTHREAD_MUTEX_INITIALIZER, 100};
static struct Account bob = {PTHREAD_MUTEX_INITIALIZER, 100};

static void transfer(struct Account* from, struct Account* to, int amount)
{
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    from->balance -= amount;
    to->balance += amount;
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}

static void payThroughEscrow(struct Account* payer, struct Account* payee, int amount)
{
    struct Account escrow = {PTHREAD_MUTEX_INITIALIZER, 0};
    transfer(payer, &escrow, amount);
    transfer(&escrow, payee, amount);
}

static void* payAlice(void* argument)
{
    (void)argument;
    transfer(&bob, &alice, 10);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, payAlice, NULL);
    payThroughEscrow(&alice, &bob, 10);
    pthread_join(thread, NULL);
    return alice.balance + bob.balance;
}
