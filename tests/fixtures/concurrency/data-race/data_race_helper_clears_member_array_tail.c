// SPDX-License-Identifier: Apache-2.0
// A helper clears a run of elements starting inside an array member, with a length known only at
// run time. The run reaches the element the worker writes, so the race is real: the extent of the
// first element is not the extent of what the helper touches.
#include <pthread.h>
#include <stddef.h>
#include <string.h>

struct Buffer
{
    int head;
    int items[8];
};

static struct Buffer buffer;

static void* worker(void* argument)
{
    (void)argument;
    buffer.items[5] += 1;
    return NULL;
}

static void clear(int* first, size_t count)
{
    memset(first, 0, count * sizeof *first);
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    clear(buffer.items + 2, (size_t)argc + 3);
    pthread_join(thread, NULL);
    return buffer.items[5];
}
