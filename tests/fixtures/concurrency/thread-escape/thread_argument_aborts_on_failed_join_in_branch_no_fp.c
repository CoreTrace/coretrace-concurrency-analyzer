// SPDX-License-Identifier: Apache-2.0
// run() starts the worker on `value` only when asked to, and aborts when joining it fails: run()
// returns only once the worker has finished.
// Expected: no diagnostic.
#include <pthread.h>
#include <stdlib.h>
static int sink;
static void* worker(void* argument)
{
    sink = *(int*)argument;
    return NULL;
}
static int run(int enabled)
{
    int value = 1;
    pthread_t thread;
    if (enabled)
    {
        pthread_create(&thread, NULL, worker, &value);
        if (pthread_join(thread, NULL) != 0)
            abort();
    }
    return sink;
}
int main(int argc, char** argv)
{
    (void)argv;
    return run(argc > 1);
}
