// SPDX-License-Identifier: Apache-2.0
// The counter decreases; before starting thread i, main writes shifted[i], which is results[i + 1],
// the element thread i + 1 (started one round earlier) may still be writing (#108).
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
    int* shifted = &results[1];
    for (int i = 3; i >= 0; --i)
    {
        shifted[i] = -1;
        pthread_create(&threads[i], NULL, worker, &results[i]);
    }
    return 0;
}
