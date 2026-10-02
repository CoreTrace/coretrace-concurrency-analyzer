// SPDX-License-Identifier: Apache-2.0
// Thread i writes view.s.data[i], which is view.flat[i + 1]; before starting thread i, main writes
// view.flat[i], the element thread i - 1 may still be writing (#108).
// Expected: one data race.
#include <pthread.h>
static union
{
    int flat[5];
    struct
    {
        int pad;
        int data[4];
    } s;
} view;
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
    {
        view.flat[i] = -1;
        pthread_create(&threads[i], NULL, worker, &view.s.data[i]);
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return view.flat[0];
}
