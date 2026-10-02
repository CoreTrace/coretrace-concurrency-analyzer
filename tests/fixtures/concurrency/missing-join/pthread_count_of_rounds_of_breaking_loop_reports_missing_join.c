// SPDX-License-Identifier: Apache-2.0
// started counts the rounds of a loop that may break early, so it may end below 4: the loop joining
// threads[0..started) then leaves some of the four threads unjoined.
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
    for (int i = 0; i < 4; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    int started = 0;
    for (int i = 0; i < 4; i++)
    {
        if (argc > 3)
            break;
        ++started;
    }
    for (int k = 0; k < started; k++)
        pthread_join(threads[k], NULL);
    return 0;
}
