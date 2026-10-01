// SPDX-License-Identifier: Apache-2.0
// Readers are joined in a loop that only reports a failed join and goes on, then main starts the
// writer: a reader whose join failed may still read `shared` while it writes. Every reader's
// handle is still joined.
// Expected: one data race on `shared`, a reader against the writer.
#include <pthread.h>
#include <stdio.h>
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
            perror("pthread_join");
    puts("readers joined");
    pthread_create(&last, NULL, writer, NULL);
    pthread_join(last, NULL);
    return 0;
}
