// SPDX-License-Identifier: Apache-2.0
// Each round starts a reader and joins it before main writes again: no reader runs beside a
// write (#113).
// Expected: no diagnostic.
#include <pthread.h>
static int shared;
static void* reader(void* argument)
{
    (void)argument;
    return (void*)(long)shared;
}
int main(void)
{
    for (int round = 0; round < 4; ++round)
    {
        shared = round;
        pthread_t thread;
        pthread_create(&thread, NULL, reader, NULL);
        pthread_join(thread, NULL);
        shared = -round;
    }
    return shared;
}
