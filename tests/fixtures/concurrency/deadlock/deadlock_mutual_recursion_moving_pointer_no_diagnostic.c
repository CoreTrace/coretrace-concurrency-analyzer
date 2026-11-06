// SPDX-License-Identifier: Apache-2.0
// Hand-over-hand locking of an array by two functions calling each other: each locks its node,
// then hands the next one to the other while still holding it. Both threads walk the array in the
// same direction. Every call names a lock further along the array, through the two functions in
// turn, so the orders they pass each other must stop somewhere (#242).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Node
{
    pthread_mutex_t lock;
};

static struct Node nodes[4] = {{PTHREAD_MUTEX_INITIALIZER},
                               {PTHREAD_MUTEX_INITIALIZER},
                               {PTHREAD_MUTEX_INITIALIZER},
                               {PTHREAD_MUTEX_INITIALIZER}};

static void lockOdd(struct Node* node, int remaining);

static void lockEven(struct Node* node, int remaining)
{
    pthread_mutex_lock(&node->lock);
    if (remaining > 1)
        lockOdd(node + 1, remaining - 1);
    pthread_mutex_unlock(&node->lock);
}

static void lockOdd(struct Node* node, int remaining)
{
    pthread_mutex_lock(&node->lock);
    if (remaining > 1)
        lockEven(node + 1, remaining - 1);
    pthread_mutex_unlock(&node->lock);
}

static void* walker(void* argument)
{
    (void)argument;
    lockEven(&nodes[0], 4);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, walker, NULL);
    lockEven(&nodes[0], 4);
    pthread_join(thread, NULL);
    return 0;
}
