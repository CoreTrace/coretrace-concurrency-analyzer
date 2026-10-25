// SPDX-License-Identifier: Apache-2.0
// starter, a thread entry that also calls itself, starts first while second runs. Called, it is
// no root, and no caller expands it: its instance of first is weighed by nothing (#224).
// Expected: one data race, first against second.
#include <pthread.h>
#include <stddef.h>

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

static void* starter(void* argument)
{
    pthread_t child;
    pthread_create(&child, NULL, first, NULL);
    pthread_join(child, NULL);
    if (argument != NULL)
        starter(NULL);
    return NULL;
}

int main(void)
{
    pthread_t a;
    pthread_t b;
    pthread_t s;
    pthread_create(&a, NULL, first, NULL);
    pthread_join(a, NULL);
    pthread_create(&b, NULL, second, NULL);
    pthread_create(&s, NULL, starter, &s);
    pthread_join(b, NULL);
    pthread_join(s, NULL);
    return shared;
}
