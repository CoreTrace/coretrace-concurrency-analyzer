// SPDX-License-Identifier: Apache-2.0
// The handles of the created threads fill threads[0..started): a creation that fails leaves its
// slot to the next one. The second loop joins them all (#157).
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
    {
        if (pthread_create(&threads[started], NULL, worker, NULL) == 0)
            ++started;
    }
    for (int k = 0; k < started; ++k)
        pthread_join(threads[k], NULL);
    return started;
}
