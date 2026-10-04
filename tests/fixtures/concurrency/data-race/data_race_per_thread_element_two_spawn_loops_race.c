// SPDX-License-Identifier: Apache-2.0
// Two spawn loops start the same worker on &results[i]; thread i of each loop writes results[i] at
// the same time (#108).
// Expected: one data race.
#include <pthread.h>
static int results[4];
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
int main(void)
{
    pthread_t first[4];
    pthread_t second[4];
    for (int i = 0; i < 4; ++i)
        pthread_create(&first[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
        pthread_create(&second[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
    {
        pthread_join(first[i], NULL);
        pthread_join(second[i], NULL);
    }
    return results[0];
}
