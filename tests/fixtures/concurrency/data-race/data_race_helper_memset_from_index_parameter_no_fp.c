// SPDX-License-Identifier: Apache-2.0
// clear_from() clears `count` elements of `slots` from the one its index picks, with a length
// known only at run time. main calls clear_from(1, argc) while the thread increments slots[0]:
// the clearing runs forward from slots[1] and never reaches slots[0] (#159). The builtin is the
// memory intrinsic on every target, where `memset` may become a library call.
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int slots[3];

static void clear_from(int index, int count)
{
    __builtin_memset(&slots[index], 0, (size_t)count * sizeof(int));
}

static void* worker(void* argument)
{
    (void)argument;
    slots[0] += 1;
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    clear_from(1, argc);
    pthread_join(thread, NULL);
    return slots[0];
}
