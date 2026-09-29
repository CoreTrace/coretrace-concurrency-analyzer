// SPDX-License-Identifier: Apache-2.0
// Hand-over-hand locking of an array by a recursion that moves the pointer it passes on: each call
// locks its node, then recurses into the next one while still holding it. Both threads walk the
// array in the same direction. Every recursive call names a lock further along the array, so the
// orders passed back to the callers must stop somewhere.
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

static void lockFrom(struct Node* node, int remaining)
{
    pthread_mutex_lock(&node->lock);
    if (remaining > 1)
        lockFrom(node + 1, remaining - 1);
    pthread_mutex_unlock(&node->lock);
}

static void* walker(void* argument)
{
    (void)argument;
    lockFrom(&nodes[0], 4);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, walker, NULL);
    lockFrom(&nodes[0], 4);
    pthread_join(thread, NULL);
    return 0;
}
