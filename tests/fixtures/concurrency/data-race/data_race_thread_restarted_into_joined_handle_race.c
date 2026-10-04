// SPDX-License-Identifier: Apache-2.0
// A second thread is started into the handle the first one was joined through: main's write
// before the second join runs beside it, while its write before any thread does not (#113).
// Expected: one data race.
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
    shared += 5;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_join(thread, NULL);
    pthread_create(&thread, NULL, worker, NULL);
    shared += 1;
    pthread_join(thread, NULL);
    return 0;
}
