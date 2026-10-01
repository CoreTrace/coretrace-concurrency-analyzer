// SPDX-License-Identifier: Apache-2.0
// The spawn loop may break right after a spawn, before raising started: that thread's slot is
// never counted, and the loop joining threads[0..started) leaves it unjoined.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t threads[4];
    int started = 0;
    for (int i = 0; i < 4; i++)
    {
        pthread_create(&threads[i], NULL, worker, NULL);
        if (argc > 3)
            break;
        ++started;
    }
    for (int k = 0; k < started; k++)
        pthread_join(threads[k], NULL);
    return 0;
}
