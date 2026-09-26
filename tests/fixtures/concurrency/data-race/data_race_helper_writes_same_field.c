// SPDX-License-Identifier: Apache-2.0
// The control for data_race_helper_writes_sibling_field_no_fp.c: the helper is handed the very
// field the worker writes, so the race is real and must stay reported.
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

static void set_value(int* value)
{
    *value = 1;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    set_value(&shared.counter);
    pthread_join(thread, NULL);
    return shared.counter + shared.flag;
}
