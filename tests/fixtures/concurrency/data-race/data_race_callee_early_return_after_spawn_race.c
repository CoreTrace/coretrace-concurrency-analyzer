// SPDX-License-Identifier: Apache-2.0
// The callee may return early right after starting the thread, leaving it running: the spawn
// dominates neither return, yet main's write after the call may run beside it (#113).
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
static int start(int wanted)
{
    if (!wanted)
        return -1;
    pthread_create(&thread, NULL, worker, NULL);
    return 0;
}
int main(int argc, char** argv)
{
    (void)argv;
    const int started = start(argc > 1);
    shared += 1;
    if (started == 0)
        pthread_join(thread, NULL);
    return 0;
}
