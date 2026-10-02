// SPDX-License-Identifier: Apache-2.0
// Two threads are created into threads[0] and threads[1], but the loop joins from threads[1]: the
// thread in threads[0] is never joined.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static void* other(void* argument)
{
    (void)argument;
    return NULL;
}

int main(void)
{
    pthread_t threads[2];
    pthread_create(&threads[0], NULL, other, NULL);
    pthread_create(&threads[1], NULL, other, NULL);
    for (int i = 1; i < 2; i++)
        pthread_join(threads[i], NULL);
    return 0;
}
