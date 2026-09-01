// SPDX-License-Identifier: Apache-2.0
// Regression: the loop creates one thread per iteration into the same handle while a single
// join outside the loop resolves only the last one. Balanced create/join counts hid it.
#include <pthread.h>

void* worker(void* arg)
{
    return arg;
}

int main(void)
{
    pthread_t handle;

    for (int index = 0; index < 4; ++index)
        pthread_create(&handle, 0, worker, 0);

    pthread_join(handle, 0);
    return 0;
}
