// SPDX-License-Identifier: Apache-2.0
// The same program with `first`'s handle in a global: `second` starts where joining `first`
// failed, and both write `shared`.
// Expected: one data race on `shared`, `first` against `second`.
#include <pthread.h>
static int shared;
static pthread_t a;
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
    pthread_t b;
    pthread_create(&a, NULL, first, NULL);
    if (pthread_join(a, NULL) != 0)
    {
        pthread_create(&b, NULL, second, NULL);
        pthread_join(b, NULL);
    }
    return 0;
}
