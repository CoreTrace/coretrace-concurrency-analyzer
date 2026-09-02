// SPDX-License-Identifier: Apache-2.0
// Worker unit: it owns the shared counter and the thread body, and never sees a spawn.
// On its own this unit is silent — nothing here proves two threads run `worker`.
#include <stddef.h>

int shared_counter = 0;

void* worker(void* argument)
{
    (void)argument;
    for (int i = 0; i < 1000; ++i)
        shared_counter = shared_counter + 1;

    return NULL;
}
