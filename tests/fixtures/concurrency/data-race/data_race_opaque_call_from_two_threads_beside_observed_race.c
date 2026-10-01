// SPDX-License-Identifier: Apache-2.0
// main writes `shared` while `first` writes it too: a race seen on the two stores themselves.
// Both threads also call touch(), which hands `shared` to fill(), a function without a body, so
// what the two calls do to it is only inferred. Once a race on `shared` is seen, the inferred
// effect compared with itself adds nothing but noise, as it would against any other access
// (#155).
// Expected: one data race on `shared`, main's write against `first`'s.
#include <pthread.h>
#include <stddef.h>

static int shared;

void fill(int* target);

static void touch(void)
{
    fill(&shared);
}

static void* first(void* argument)
{
    (void)argument;
    shared = 1;
    touch();
    return NULL;
}

static void* second(void* argument)
{
    (void)argument;
    touch();
    return NULL;
}

int main(void)
{
    pthread_t firstThread;
    pthread_t secondThread;
    pthread_create(&firstThread, NULL, first, NULL);
    pthread_create(&secondThread, NULL, second, NULL);
    shared = 2;
    pthread_join(firstThread, NULL);
    pthread_join(secondThread, NULL);
    return shared;
}
