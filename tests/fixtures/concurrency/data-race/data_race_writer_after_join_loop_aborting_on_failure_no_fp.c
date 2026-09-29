// SPDX-License-Identifier: Apache-2.0
// Readers are joined in a loop that aborts when a join fails, and only then does main start the
// writer: every reader has finished before it writes `shared`.
// Expected: no diagnostic.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#define READER_COUNT 4
static int shared;
static void* reader(void* argument)
{
    (void)argument;
    return (void*)(long)shared;
}
static void* writer(void* argument)
{
    (void)argument;
    shared = 1;
    return NULL;
}
int main(void)
{
    pthread_t readers[READER_COUNT];
    pthread_t last;
    for (int i = 0; i < READER_COUNT; i++)
        pthread_create(&readers[i], NULL, reader, NULL);
    for (int i = 0; i < READER_COUNT; i++)
        if (pthread_join(readers[i], NULL) != 0)
            abort();
    puts("readers joined");
    pthread_create(&last, NULL, writer, NULL);
    pthread_join(last, NULL);
    return 0;
}
