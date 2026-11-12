// SPDX-License-Identifier: Apache-2.0
// The thread is handed a pointer read from memory, while the handle lies in a local whose address
// goes nowhere else: the pointer cannot lead to the handle, so the join ends the thread (#201).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Config
{
    int* data;
};

static int shared;
static int store;
static struct Config config = {&store};

static void* worker(void* argument)
{
    int* data = argument;
    *data += 1;
    shared += 1;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, config.data);
    pthread_join(thread, NULL);
    return shared;
}
