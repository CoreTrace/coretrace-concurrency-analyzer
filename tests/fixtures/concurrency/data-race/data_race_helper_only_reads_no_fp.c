// SPDX-License-Identifier: Apache-2.0
// Both threads call a helper that only reads the shared structure. Two reads never race, and what
// the call may do to the structure is what the helper does: read it.
#include <pthread.h>
#include <stddef.h>

struct Settings
{
    int width;
    int height;
};

static struct Settings settings = {640, 480};

static int area(const struct Settings* current)
{
    return current->width * current->height;
}

static void* worker(void* argument)
{
    (void)argument;
    return (void*)(size_t)area(&settings);
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    int result = area(&settings);
    pthread_join(thread, NULL);
    return result;
}
