// SPDX-License-Identifier: Apache-2.0
// The same with the handle in a local: when pthread_join fails, run() returns while the worker
// may still read `value` in its frame.
// Expected: one thread-arg-escape, at the creation.
#include <pthread.h>
static int sink;
static void* worker(void* argument)
{
    sink = *(int*)argument;
    return NULL;
}
static int run(void)
{
    int value = 1;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &value);
    if (pthread_join(thread, NULL) != 0)
        return -1;
    return sink;
}
int main(void)
{
    return run();
}
