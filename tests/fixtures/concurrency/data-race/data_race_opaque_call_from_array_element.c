// SPDX-License-Identifier: Apache-2.0
// The same pointer into the middle of an array, handed directly to a function whose body is not
// in the unit. What that function may reach runs past the element it is given, up to the one the
// worker writes.
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

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    refill(&values[2], (size_t)argc + 3);
    pthread_join(thread, NULL);
    return values[5];
}
