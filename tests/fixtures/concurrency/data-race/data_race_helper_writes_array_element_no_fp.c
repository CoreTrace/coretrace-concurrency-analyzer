// SPDX-License-Identifier: Apache-2.0
// A helper writes exactly the element it is handed. The write has a known size, so it stays at
// that element rather than spreading over the array the element belongs to, and the worker's
// write to another element does not overlap it.
#include <pthread.h>
#include <stddef.h>

static int values[8];

static void* worker(void* argument)
{
    (void)argument;
    values[0] += 1;
    return NULL;
}

static void set_one(int* slot)
{
    *slot = 1;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    set_one(&values[2]);
    pthread_join(thread, NULL);
    return values[0];
}
