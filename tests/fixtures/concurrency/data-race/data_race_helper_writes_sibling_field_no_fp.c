// SPDX-License-Identifier: Apache-2.0
// A helper writes the field it is handed while the worker writes a different field of the same
// global: the two never touch the same bytes. The helper's access is relative to its parameter,
// and the caller's argument names the field at its offset, so the pair must not overlap.
#include <pthread.h>
#include <stddef.h>

struct Shared
{
    int counter;
    int flag;
};

static struct Shared shared;

static void* worker(void* argument)
{
    (void)argument;
    shared.counter += 1;
    return NULL;
}

static void set_flag(int* flag)
{
    *flag = 1;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    set_flag(&shared.flag);
    pthread_join(thread, NULL);
    return shared.counter + shared.flag;
}
