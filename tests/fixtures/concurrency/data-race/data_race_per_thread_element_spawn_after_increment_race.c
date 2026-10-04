// SPDX-License-Identifier: Apache-2.0
// Round k writes results[k] and then starts a thread on results[k + 1]; results[k] belongs to the
// thread round k - 1 started, which may still run (#108).
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
    int i = 0;
    while (i < 4)
    {
        results[i] = -1;
        i++;
        pthread_create(&threads[i - 1], NULL, worker, &results[i]);
    }
    return 0;
}
