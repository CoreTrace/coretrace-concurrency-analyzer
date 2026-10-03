// SPDX-License-Identifier: Apache-2.0
// Workers write their own word of a union; a second thread writes pair[1], which overlays word[2]
// and word[3] (#108).
// Expected: one data race.
#include <pthread.h>
static union
{
    int word[4];
    long long pair[2];
} shared;
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
static void* overlay(void* argument)
{
    (void)argument;
    shared.pair[1] = 0;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    pthread_t other;
    pthread_create(&other, NULL, overlay, NULL);
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &shared.word[i]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    pthread_join(other, NULL);
    return shared.word[0];
}
