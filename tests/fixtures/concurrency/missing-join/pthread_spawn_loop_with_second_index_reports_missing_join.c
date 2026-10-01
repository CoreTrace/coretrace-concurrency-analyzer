// SPDX-License-Identifier: Apache-2.0
// The spawn loop stores at threads[i][column], column read from the program's arguments, but the
// join loop walks column 0: when column is 1, no thread started is ever joined.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t threads[2][2];
    int column = argc > 1;
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[i][column], NULL, worker, NULL);
    for (int i = 0; i < 2; i++)
        pthread_join(threads[i][0], NULL);
    return 0;
}
