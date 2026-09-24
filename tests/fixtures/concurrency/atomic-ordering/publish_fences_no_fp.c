// SPDX-License-Identifier: Apache-2.0
// The flag operations themselves are relaxed, and the fences carry the ordering: a release fence
// before the store and an acquire fence after the load that observed it order the payload
// accesses as release and acquire operations would.
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

static int payload = 0;
static atomic_bool ready = false;
static int observed = 0;

static void* producer(void* argument)
{
    (void)argument;
    payload = 42;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&ready, true, memory_order_relaxed);
    return NULL;
}

static void* consumer(void* argument)
{
    (void)argument;
    while (!atomic_load_explicit(&ready, memory_order_relaxed))
        ;
    atomic_thread_fence(memory_order_acquire);
    observed = payload;
    return NULL;
}

int main(void)
{
    pthread_t first;
    pthread_t second;
    pthread_create(&first, NULL, producer, NULL);
    pthread_create(&second, NULL, consumer, NULL);
    pthread_join(first, NULL);
    pthread_join(second, NULL);
    return observed;
}
