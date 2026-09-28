// SPDX-License-Identifier: Apache-2.0
// A pointer into an array member reaches that array, not the field declared next to it: the
// worker's write to `head` does not overlap anything the function without a body may touch.
#include <pthread.h>
#include <stddef.h>

struct Buffer
{
    int head;
    int items[8];
};

static struct Buffer buffer;

void refill(int* first, size_t count);

static void* worker(void* argument)
{
    (void)argument;
    buffer.head += 1;
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
    fill_from(buffer.items + 2, (size_t)argc + 3);
    pthread_join(thread, NULL);
    return buffer.head;
}
