// SPDX-License-Identifier: Apache-2.0
// The thread is joined on one branch only: main's write after the branches may run beside it
// (#113).
// Expected: one data race.
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
    if (argc > 1)
    {
        pthread_t thread;
        pthread_create(&thread, NULL, worker, NULL);
        shared += 1;
        pthread_join(thread, NULL);
    }
    shared += 2;
    return shared;
}
