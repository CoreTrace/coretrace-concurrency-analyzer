// SPDX-License-Identifier: Apache-2.0
// main keeps the join's result in a local it never looks at: the result is ignored, as when it is
// not kept at all, and the read comes after the join.
// Expected: no diagnostic.
#include <pthread.h>
static int shared;
static pthread_t thread;
static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    int status = pthread_join(thread, NULL);
    (void)status;
    return shared;
}
