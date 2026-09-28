// SPDX-License-Identifier: Apache-2.0
// A pointer to an element may be indexed backwards as well as forwards, so a function without a
// body handed &values[2] may reach values[0], which the worker writes. What it reaches is the
// whole array, not the elements from the pointer on.
#include <pthread.h>
#include <stddef.h>

static int values[8];

void refill(int* first, size_t count);

static void* worker(void* argument)
{
    (void)argument;
    values[0] += 1;
    return NULL;
}

static void fill_from(int* first, size_t count)
{
    refill(first, count);
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    fill_from(&values[2], (size_t)argc + 3);
    pthread_join(thread, NULL);
    return values[0];
}
