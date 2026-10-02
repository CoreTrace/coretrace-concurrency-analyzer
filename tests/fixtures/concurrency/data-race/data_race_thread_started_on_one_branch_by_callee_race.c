// SPDX-License-Identifier: Apache-2.0
// The callee starts the thread on one branch only and leaves it running: main's write after the
// call may run beside it (#113).
// Expected: one data race.
#include <pthread.h>
static int shared;
static pthread_t thread;
static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
static void maybeStart(int wanted)
{
    if (wanted)
        pthread_create(&thread, NULL, worker, NULL);
}
int main(int argc, char** argv)
{
    (void)argv;
    maybeStart(argc > 1);
    shared += 1;
    if (argc > 1)
        pthread_join(thread, NULL);
    return 0;
}
