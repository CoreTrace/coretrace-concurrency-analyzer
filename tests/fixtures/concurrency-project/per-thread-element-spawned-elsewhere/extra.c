// SPDX-License-Identifier: Apache-2.0
// Starts worker on &results[0], an element main's spawn loop also hands to a thread (#108).
#include <pthread.h>
extern int results[4];
void* worker(void* argument);
static pthread_t extra;
void start_extra(void)
{
    pthread_create(&extra, NULL, worker, &results[0]);
}
void join_extra(void)
{
    pthread_join(extra, NULL);
}
