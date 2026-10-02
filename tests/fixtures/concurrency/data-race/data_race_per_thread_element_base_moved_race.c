// SPDX-License-Identifier: Apache-2.0
// The base pointer moves back one element after round 0, so rounds 0 and 1 both hand &pool[1] to a
// thread (#108).
// Expected: one data race.
#include <pthread.h>
static int pool[8];
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    int* base = &pool[1];
    for (int i = 0; i < 4; ++i)
    {
        pthread_create(&threads[i], NULL, worker, &base[i]);
        base = &pool[0];
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return pool[1];
}
