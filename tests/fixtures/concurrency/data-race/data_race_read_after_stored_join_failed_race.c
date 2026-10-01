// SPDX-License-Identifier: Apache-2.0
// main keeps the join's result in a local and reads `shared` on the branch where it reports a
// failure: the worker may still be running there.
// Expected: one data race on `shared`, main's read against the worker's write.
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
    if (status != 0)
        return shared;
    return 0;
}
