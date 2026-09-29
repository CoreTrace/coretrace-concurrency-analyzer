// SPDX-License-Identifier: Apache-2.0
// The helper locks the account it is handed, starts the auditor, then takes first then second; the
// auditor takes second then first. The account's lock around the helper's order sends it to main's
// call, where no thread runs yet: the auditor the helper starts still runs beside it (#136).
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

struct Account
{
    pthread_mutex_t lock;
    int balance;
};

static struct Account alice = {PTHREAD_MUTEX_INITIALIZER, 100};
static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void* audit(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&second);
    pthread_mutex_lock(&first);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&second);
    return NULL;
}

static void transferWhileAuditing(struct Account* account)
{
    pthread_t thread;
    pthread_mutex_lock(&account->lock);
    pthread_create(&thread, NULL, audit, NULL);
    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&account->lock);
    pthread_join(thread, NULL);
}

int main(void)
{
    transferWhileAuditing(&alice);
    return 0;
}
