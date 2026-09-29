// SPDX-License-Identifier: Apache-2.0
// Only one branch of the `if` joins the thread before run() returns: on the other, the worker may
// still read `value` in the frame run() has left.
// Expected: one thread-arg-escape. The missing join and the race on `sink` are reported too.
#include <pthread.h>
static int sink;
static void* worker(void* argument)
{
    sink = *(int*)argument;
    return NULL;
}
static int run(int verbose)
{
    int value = 1;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &value);
    if (verbose)
        pthread_join(thread, NULL);
    return sink;
}
int main(int argc, char** argv)
{
    (void)argv;
    return run(argc > 1);
}
