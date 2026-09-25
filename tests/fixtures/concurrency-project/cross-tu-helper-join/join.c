// SPDX-License-Identifier: Apache-2.0
// Joining unit: it waits for whichever thread it is handed.
#include <pthread.h>
#include <stddef.h>

void join_worker(pthread_t thread)
{
    pthread_join(thread, NULL);
}
