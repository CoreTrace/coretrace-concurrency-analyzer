// SPDX-License-Identifier: Apache-2.0
// The counter decreases; before starting thread i, main writes 8 bytes at &results[i], reaching
// results[i + 1], which thread i + 1 (started one round earlier) may still be writing (#108).
// Nothing joins the threads: their join loop would not be matched (#173).
// Expected: one data race and one missing join.
#include <pthread.h>
static int results[5];
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    for (int i = 3; i >= 0; --i)
    {
        *(long long*)&results[i] = 0;
        pthread_create(&threads[i], NULL, worker, &results[i]);
    }
    return 0;
}
