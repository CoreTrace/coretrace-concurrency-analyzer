// SPDX-License-Identifier: Apache-2.0
// The inverted transfers reach the locking helper ten calls down, through nine functions that
// forward both accounts. Each function is defined before the one it calls, so the orders the helper
// takes reach main one level at a time.
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

struct Account
{
    pthread_mutex_t lock;
    int balance;
};

static struct Account alice = {PTHREAD_MUTEX_INITIALIZER, 100};
static struct Account bob = {PTHREAD_MUTEX_INITIALIZER, 100};

static void level1(struct Account* from, struct Account* to);
static void level2(struct Account* from, struct Account* to);
static void level3(struct Account* from, struct Account* to);
static void level4(struct Account* from, struct Account* to);
static void level5(struct Account* from, struct Account* to);
static void level6(struct Account* from, struct Account* to);
static void level7(struct Account* from, struct Account* to);
static void level8(struct Account* from, struct Account* to);
static void level9(struct Account* from, struct Account* to);
static void transfer(struct Account* from, struct Account* to);

static void* payAlice(void* argument)
{
    (void)argument;
    level1(&bob, &alice);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, payAlice, NULL);
    level1(&alice, &bob);
    pthread_join(thread, NULL);
    return alice.balance + bob.balance;
}

static void level1(struct Account* from, struct Account* to) { level2(from, to); }
static void level2(struct Account* from, struct Account* to) { level3(from, to); }
static void level3(struct Account* from, struct Account* to) { level4(from, to); }
static void level4(struct Account* from, struct Account* to) { level5(from, to); }
static void level5(struct Account* from, struct Account* to) { level6(from, to); }
static void level6(struct Account* from, struct Account* to) { level7(from, to); }
static void level7(struct Account* from, struct Account* to) { level8(from, to); }
static void level8(struct Account* from, struct Account* to) { level9(from, to); }
static void level9(struct Account* from, struct Account* to) { transfer(from, to); }

static void transfer(struct Account* from, struct Account* to)
{
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    from->balance -= 1;
    to->balance += 1;
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}
