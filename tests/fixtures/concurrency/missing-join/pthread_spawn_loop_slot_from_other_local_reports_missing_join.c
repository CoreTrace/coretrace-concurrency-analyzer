// SPDX-License-Identifier: Apache-2.0
// The spawn loop counts with i but stores every handle at threads[slot], slot staying 0: each
// round overwrites the last handle, and only the final thread is ever joined.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    int slot = 0;
    for (int i = 0; i < 4; i++)
        pthread_create(&threads[slot], NULL, worker, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(threads[i], NULL);
    return 0;
}
