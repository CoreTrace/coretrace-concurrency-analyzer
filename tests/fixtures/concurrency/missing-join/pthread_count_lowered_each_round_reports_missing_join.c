// SPDX-License-Identifier: Apache-2.0
// count is lowered by one each round, not raised, so it ends at -4: the loop joining
// threads[0..count) never runs, and none of the four threads is joined.
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
    for (int i = 0; i < 4; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    int count = 0;
    for (int i = 0; i < 4; i++)
        count -= 1;
    for (int k = 0; k < count; k++)
        pthread_join(threads[k], NULL);
    return 0;
}
