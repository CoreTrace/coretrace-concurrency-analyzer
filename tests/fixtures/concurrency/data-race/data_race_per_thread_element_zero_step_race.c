// SPDX-License-Identifier: Apache-2.0
// The counter's step is zero; the loop stops after two spawns, both handed &results[0] (#108).
// Nothing joins the threads: their join loop would not be matched (#173).
// Expected: one data race and one missing join.
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
    pthread_t threads[2];
    int started = 0;
    for (int i = 0; i < 4; i += 0)
    {
        pthread_create(&threads[started], NULL, worker, &results[i]);
        if (++started == 2)
            break;
    }
    return 0;
}
