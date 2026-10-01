// SPDX-License-Identifier: Apache-2.0
// The same program with the handle in a global: main reads `shared` on the branch where
// pthread_join reports a failure, while the worker may still run.
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
    if (pthread_join(thread, NULL) != 0)
        return shared;
    return 0;
}
