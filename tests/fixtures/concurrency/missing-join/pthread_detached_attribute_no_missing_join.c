// SPDX-License-Identifier: Apache-2.0
// Regression: a thread created with PTHREAD_CREATE_DETACHED never needs a join. The
// attribute was never inspected. Its enum value differs per platform and is resolved from
// the module's target triple.
#include <pthread.h>

void* worker(void* arg)
{
    return arg;
}

int main(void)
{
    pthread_attr_t attributes;
    pthread_t handle;

    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_create(&handle, &attributes, worker, 0);
    pthread_attr_destroy(&attributes);
    return 0;
}
