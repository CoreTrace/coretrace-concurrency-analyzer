// SPDX-License-Identifier: Apache-2.0
// Each round starts writerA and writerB on the same &results[i]; both write results[i] at the same
// time (#108).
// Expected: one data race.
#include <pthread.h>
static int results[4];
static void* writerA(void* argument)
{
    int* slot = argument;
    *slot = 1;
    return NULL;
}
static void* writerB(void* argument)
{
    int* slot = argument;
    *slot = 2;
    return NULL;
}
int main(void)
{
    pthread_t a[4];
    pthread_t b[4];
    for (int i = 0; i < 4; ++i)
    {
        pthread_create(&a[i], NULL, writerA, &results[i]);
        pthread_create(&b[i], NULL, writerB, &results[i]);
    }
    for (int i = 0; i < 4; ++i)
    {
        pthread_join(a[i], NULL);
        pthread_join(b[i], NULL);
    }
    return results[0];
}
