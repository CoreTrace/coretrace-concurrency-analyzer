// SPDX-License-Identifier: Apache-2.0
// After joining thread i, the join loop clears 8 bytes from results[i]: they reach results[i + 1],
// which thread i + 1 may still be writing (#108).
// Expected: one data race.
#include <pthread.h>

static int results[4];

static void* worker(void* argument)
{
    int* element = argument;
    *element = 42;
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
    {
        pthread_join(threads[i], NULL);
        if (i + 1 < 4)
            *(long long*)&results[i] = 0;
    }
    return 0;
}
