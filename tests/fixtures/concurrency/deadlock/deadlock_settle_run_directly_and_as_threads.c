// SPDX-License-Identifier: Apache-2.0
// settle locks the account it is handed, then first and second. main settles alice itself, before
// any thread exists, then starts settle as a thread for bob and for carol, and takes second then
// first while they run. The threads run settle's order where no call does (#136).
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
static struct Account carol = {PTHREAD_MUTEX_INITIALIZER, 100};
static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void* settle(void* argument)
{
    struct Account* account = argument;
    pthread_mutex_lock(&account->lock);
    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&account->lock);
    return NULL;
}

int main(void)
{
    settle(&alice);
    pthread_t forBob;
    pthread_t forCarol;
    pthread_create(&forBob, NULL, settle, &bob);
    pthread_create(&forCarol, NULL, settle, &carol);
    pthread_mutex_lock(&second);
    pthread_mutex_lock(&first);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&second);
    pthread_join(forBob, NULL);
    pthread_join(forCarol, NULL);
    return 0;
}
