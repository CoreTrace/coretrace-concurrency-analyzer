// SPDX-License-Identifier: Apache-2.0
// run() returns without joining while the worker may still read `value` in its frame, and
// reads `sink` while the worker writes it.
// Expected: one thread-arg-escape at the creation, one missing join, and one data race on `sink`.
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
    return sink;
}
int main(void)
{
    return run();
}
