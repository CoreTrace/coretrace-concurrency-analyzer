// SPDX-License-Identifier: Apache-2.0
// Both threads update a field of the shared object through a helper that takes a global mutex
// around the write. What the helper does to its argument happens under that mutex, and the call
// sites, which hold nothing, must not report it as an unprotected write of their own.
#include <pthread.h>
#include <stddef.h>

struct Tally
{
    int count;
};

static struct Tally tally;
static pthread_mutex_t tally_lock = PTHREAD_MUTEX_INITIALIZER;

static void add_one(struct Tally* target)
{
    pthread_mutex_lock(&tally_lock);
    target->count += 1;
    pthread_mutex_unlock(&tally_lock);
}

static void* worker(void* argument)
{
    (void)argument;
    add_one(&tally);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    add_one(&tally);
    pthread_join(thread, NULL);
    return tally.count;
}
