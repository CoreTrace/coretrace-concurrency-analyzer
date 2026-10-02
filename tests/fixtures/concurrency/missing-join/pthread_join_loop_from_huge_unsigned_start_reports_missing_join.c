// SPDX-License-Identifier: Apache-2.0
// The join loop starts its unsigned counter at 0xFFFFFFFE, which is not below 4: it never runs,
// and no thread is joined. Read as a signed constant, that start would be -2.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    for (unsigned i = 0; i < 4u; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    for (unsigned i = 0xFFFFFFFEu; i < 4u; i++)
        pthread_join(threads[i], NULL);
    return 0;
}
