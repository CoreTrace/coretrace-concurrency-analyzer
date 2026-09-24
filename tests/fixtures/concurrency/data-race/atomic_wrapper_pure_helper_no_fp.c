// SPDX-License-Identifier: Apache-2.0
// Every access to the flag is atomic, made through wrappers that also call a helper touching no
// shared memory at all. The helper does not make the wrappers' accesses plain: this is the shape
// a standard library gives its atomic members when it computes the order through a function.
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

static atomic_bool ready = false;

static int order_mask(int order)
{
    return order & 0xffff;
}

static void set_ready(atomic_bool* flag, int order)
{
    if (order_mask(order) == 0)
        atomic_store_explicit(flag, true, memory_order_relaxed);
    else
        atomic_store_explicit(flag, true, memory_order_seq_cst);
}

static bool is_ready(atomic_bool* flag, int order)
{
    if (order_mask(order) == 0)
        return atomic_load_explicit(flag, memory_order_relaxed);
    return atomic_load_explicit(flag, memory_order_seq_cst);
}

static void* producer(void* argument)
{
    (void)argument;
    set_ready(&ready, 5);
    return NULL;
}

static void* consumer(void* argument)
{
    (void)argument;
    while (!is_ready(&ready, 5))
        ;
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
    return 0;
}
