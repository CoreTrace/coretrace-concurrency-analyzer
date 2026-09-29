// SPDX-License-Identifier: Apache-2.0
// run() starts the worker on `value` only when asked to, and returns -1 when joining it fails:
// the worker may then still read `value` in the frame run() has left.
// Expected: one thread-arg-escape, at the creation.
#include <pthread.h>
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
            return -1;
    }
    return sink;
}
int main(int argc, char** argv)
{
    (void)argv;
    return run(argc > 1);
}
