// SPDX-License-Identifier: Apache-2.0
// Thread i owns view.wide[i] (8 bytes); before starting thread i, main writes view.narrow[i], which
// lies in view.wide[i / 2], owned by an earlier thread for i >= 1 (#108).
// Expected: one data race.
#include <pthread.h>
static union
{
    long long wide[4];
    int narrow[8];
} view;
static void* worker(void* argument)
{
    long long* slot = argument;
    *slot = 42;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
    {
        view.narrow[i] = -1;
        pthread_create(&threads[i], NULL, worker, &view.wide[i]);
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return view.narrow[0];
}
