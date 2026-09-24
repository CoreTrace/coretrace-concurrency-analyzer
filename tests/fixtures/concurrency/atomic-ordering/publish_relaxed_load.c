// SPDX-License-Identifier: Apache-2.0
// The flag is stored with release, but observed with a relaxed load and no acquire fence after
// it, so the reader is not ordered after the payload write. The payload is atomic: no data race,
// only a publication the reader does not synchronize with.
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

static atomic_int payload = 0;
static atomic_bool ready = false;
static int observed = 0;

static void* producer(void* argument)
{
    (void)argument;
    atomic_store_explicit(&payload, 42, memory_order_relaxed);
    atomic_store_explicit(&ready, true, memory_order_release);
    return NULL;
}

static void* consumer(void* argument)
{
    (void)argument;
    while (!atomic_load_explicit(&ready, memory_order_relaxed))
        ;
    observed = atomic_load_explicit(&payload, memory_order_relaxed);
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
