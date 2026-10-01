// SPDX-License-Identifier: Apache-2.0
// The thread is joined once, before run() returns.
// Expected: no diagnostic.
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
    (void)verbose;
    pthread_join(thread, NULL);
    return sink;
}
int main(int argc, char** argv)
{
    (void)argv;
    return run(argc > 1);
}
