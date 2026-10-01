// SPDX-License-Identifier: Apache-2.0
// The second spawn loop fills threads[1..3) over the first one's threads[0..2): the thread the
// first loop stored in threads[1] is lost, though a loop joins threads[0..3).
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
    pthread_t threads[3];
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 1; i < 3; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 0; i < 3; i++)
        pthread_join(threads[i], NULL);
    return 0;
}
