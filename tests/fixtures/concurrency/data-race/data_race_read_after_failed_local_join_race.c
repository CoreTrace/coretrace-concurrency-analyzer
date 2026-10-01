// SPDX-License-Identifier: Apache-2.0
// main starts and joins the worker through a local handle, and reads `shared` on the branch where
// pthread_join reports a failure: nothing waited for the worker there, and it may still run.
// Expected: one data race on `shared`, main's read against the worker's write.
#include <pthread.h>
static int shared;
static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    if (pthread_join(thread, NULL) != 0)
        return shared;
    return 0;
}
