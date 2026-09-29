// SPDX-License-Identifier: Apache-2.0
// Each branch of the `if` joins the thread before run() returns, so the frame outlives it,
// although neither join dominates the return.
// Expected: no thread-arg-escape. The missing join (#143) and the race on `sink` (#113) are
// reported elsewhere.
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
    else
        pthread_join(thread, NULL);
    return sink;
}
int main(int argc, char** argv)
{
    (void)argv;
    return run(argc > 1);
}
