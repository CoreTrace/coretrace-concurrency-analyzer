// SPDX-License-Identifier: Apache-2.0
// Each worker publishes its element's address in `published` (race on published), and an observer
// thread writes through it, into an element a worker is writing (race on results) (#108).
// Expected: two data races.
#include <pthread.h>
static int results[4];
static int* volatile published;
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    published = slot;
    return NULL;
}
static void* observer(void* argument)
{
    (void)argument;
    int* seen = published;
    if (seen != NULL)
        *seen = 0;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    pthread_t watch;
    pthread_create(&watch, NULL, observer, NULL);
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    pthread_join(watch, NULL);
    return 0;
}
