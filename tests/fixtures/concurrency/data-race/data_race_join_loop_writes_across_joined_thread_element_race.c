// SPDX-License-Identifier: Apache-2.0
// After joining thread i, the join loop writes 8 bytes from slots[i].second: they reach
// slots[i + 1].first, which thread i + 1 may still be writing (#108).
// Expected: one data race.
#include <pthread.h>

struct slot
{
    int first;
    int second;
};

static struct slot slots[4];

static void* worker(void* argument)
{
    struct slot* own = argument;
    own->first = 1;
    own->second = 2;
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &slots[i]);
    for (int i = 0; i < 4; ++i)
    {
        pthread_join(threads[i], NULL);
        if (i + 1 < 4)
            *(long long*)&slots[i].second = 0;
    }
    return 0;
}
