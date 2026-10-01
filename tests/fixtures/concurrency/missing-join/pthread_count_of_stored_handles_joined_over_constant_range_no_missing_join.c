// SPDX-License-Identifier: Apache-2.0
// The spawn loop stores each handle at threads[started++], four rounds from 0: the handles fill
// threads[0..4), and a loop over that constant range joins them all (#157).
// Expected: no diagnostic.
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
    int started = 0;
    for (int i = 0; i < 4; i++)
        pthread_create(&threads[started++], NULL, worker, NULL);
    for (int k = 0; k < 4; k++)
        pthread_join(threads[k], NULL);
    return 0;
}
