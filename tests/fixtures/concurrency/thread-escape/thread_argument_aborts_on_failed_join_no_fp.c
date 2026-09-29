// SPDX-License-Identifier: Apache-2.0
// run() returns only after a successful join; a failed join aborts.
// Expected: no diagnostic.
#include <pthread.h>
#include <stdlib.h>
static int sink;
static pthread_t thread;
static void* worker(void* argument)
{
    sink = *(int*)argument;
    return NULL;
}
static int run(void)
{
    int value = 1;
    pthread_create(&thread, NULL, worker, &value);
    if (pthread_join(thread, NULL) != 0)
        abort();
    return sink;
}
int main(void)
{
    return run();
}
