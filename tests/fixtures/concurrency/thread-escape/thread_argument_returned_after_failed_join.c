// SPDX-License-Identifier: Apache-2.0
// When pthread_join fails, run() returns while the worker may still read `value` in its frame.
// The handle is a global, so no completion proof applies and only the rule's own join test
// decides.
// Expected: one thread-arg-escape, at the creation.
#include <pthread.h>
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
        return -1;
    return sink;
}
int main(void)
{
    return run();
}
