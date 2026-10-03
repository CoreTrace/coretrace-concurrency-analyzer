// SPDX-License-Identifier: Apache-2.0
// After the second spawn a helper rewinds the counter through its address, so results[0] and
// results[1] are each handed to two threads (#108).
// Expected: one data race.
#include <pthread.h>
static int results[4];
static int rewound;
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
static void rewind_once(int* counter)
{
    if (*counter == 1 && !rewound)
    {
        rewound = 1;
        *counter = -1;
    }
}
int main(void)
{
    pthread_t threads[6];
    int started = 0;
    for (int i = 0; i < 4; ++i)
    {
        pthread_create(&threads[started++], NULL, worker, &results[i]);
        rewind_once(&i);
    }
    for (int k = 0; k < started; ++k)
        pthread_join(threads[k], NULL);
    return results[0];
}
