// SPDX-License-Identifier: Apache-2.0
// A call made on one branch starts the thread and leaves it running: main's write after the
// branches may run beside it (#113).
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
static void start(void)
{
    pthread_create(&thread, NULL, worker, NULL);
}
int main(int argc, char** argv)
{
    (void)argv;
    if (argc > 1)
        start();
    shared += 1;
    if (argc > 1)
        pthread_join(thread, NULL);
    return 0;
}
