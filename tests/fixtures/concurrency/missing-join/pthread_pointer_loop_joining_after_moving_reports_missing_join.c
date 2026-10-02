// SPDX-License-Identifier: Apache-2.0
// The pointer loop moves its cursor before joining through it: it joins threads[1], never filled,
// and the thread created into threads[0] is never joined.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static void* other(void* argument)
{
    (void)argument;
    return NULL;
}

int main(void)
{
    pthread_t threads[2];
    pthread_create(&threads[0], NULL, other, NULL);
    for (pthread_t* handle = threads; handle != threads + 1;)
    {
        ++handle;
        pthread_join(*handle, NULL);
    }
    return 0;
}
