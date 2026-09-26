// SPDX-License-Identifier: Apache-2.0
// A helper hands a pointer into the middle of an array to a function whose body is not in the
// unit, together with a count. That function may walk to any later element, including the one the
// worker writes: an element pointer does not bound what its receiver reaches the way a pointer to
// a field does.
#include <pthread.h>
#include <stddef.h>

static int values[8];

void refill(int* first, size_t count);

static void* worker(void* argument)
{
    (void)argument;
    values[5] += 1;
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
    return values[5];
}
