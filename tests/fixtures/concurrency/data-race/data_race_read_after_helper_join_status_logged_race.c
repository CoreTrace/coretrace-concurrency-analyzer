// SPDX-License-Identifier: Apache-2.0
// stop() logs the status of its join before returning it, and main reads `shared` only on the
// branch where that status reports success. The program is correct, but only a status returned
// exactly as the join gave it is followed through the call, so the read is still reported.
// Expected: one data race on `shared`, main's read against the worker's write.
#include <pthread.h>
#include <stdio.h>
static int shared;
static pthread_t thread;
static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
static int stop(void)
{
    int status = pthread_join(thread, NULL);
    printf("pthread_join: %d\n", status);
    return status;
}
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    if (stop() == 0)
        return shared;
    return 0;
}
