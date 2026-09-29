// SPDX-License-Identifier: Apache-2.0
// main starts `second` on the branch where joining `first` failed: `first` may still run while
// `second` does, and both write `shared`.
// Expected: one data race on `shared`, `first` against `second`.
#include <pthread.h>
static int shared;
static void* first(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
static void* second(void* argument)
{
    (void)argument;
    shared += 2;
    return NULL;
}
int main(void)
{
    pthread_t a;
    pthread_t b;
    pthread_create(&a, NULL, first, NULL);
    if (pthread_join(a, NULL) != 0)
    {
        pthread_create(&b, NULL, second, NULL);
        pthread_join(b, NULL);
    }
    return 0;
}
