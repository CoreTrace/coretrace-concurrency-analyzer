// SPDX-License-Identifier: Apache-2.0
// Shutdown unit: it joins the handle the other unit created.
#include <pthread.h>
#include <stddef.h>

extern pthread_t g_worker;

void stop_worker(void)
{
    pthread_join(g_worker, NULL);
}
