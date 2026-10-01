// SPDX-License-Identifier: Apache-2.0
// The count starts at 1, so three rounds store the handles at threads[1..4), but the join loop
// walks threads[0..3): the thread at threads[3] is never joined.
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
    int started = 1;
    for (int i = 0; i < 3; i++)
        pthread_create(&threads[started++], NULL, worker, NULL);
    for (int k = 0; k < 3; k++)
        pthread_join(threads[k], NULL);
    return 0;
}
