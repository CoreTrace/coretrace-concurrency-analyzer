// SPDX-License-Identifier: Apache-2.0
// The thread starts when argc > 1; main writes shared before an early return taken when
// argc > 2, which leaves the thread running, and once more before the join (#113).
// Expected: one data race (both writes of main) and one missing join.
#include <pthread.h>
static int shared;
static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
int main(int argc, char** argv)
{
    (void)argv;
    pthread_t thread;
    if (argc > 1)
        pthread_create(&thread, NULL, worker, NULL);
    if (argc > 2)
    {
        shared += 1;
        return 1;
    }
    shared += 2;
    if (argc > 1)
        pthread_join(thread, NULL);
    return 0;
}
