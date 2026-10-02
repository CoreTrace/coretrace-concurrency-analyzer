// SPDX-License-Identifier: Apache-2.0
// Each handle is stored at threads[started++]: the count moves past the slot before the thread
// starts, so every handle keeps its own slot of threads[0..started), which the second loop joins
// (#157).
// Expected: no diagnostic.
#include <pthread.h>

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    pthread_t threads[4];
    int started = 0;
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[started++], NULL, worker, NULL);
    for (int k = 0; k < started; ++k)
        pthread_join(threads[k], NULL);
    return started;
}
