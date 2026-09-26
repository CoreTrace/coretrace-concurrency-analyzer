// SPDX-License-Identifier: Apache-2.0
// The control for data_race_opaque_call_on_sibling_field_no_fp.c: the function without a body is
// handed the field the worker writes, so the race is real and must stay reported.
#include <pthread.h>
#include <stddef.h>

struct Shared
{
    int counter;
    int flag;
};

static struct Shared shared;

void publish(int* value);

static void* worker(void* argument)
{
    (void)argument;
    shared.counter += 1;
    return NULL;
}

static void hand_over(int* value)
{
    publish(value);
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    hand_over(&shared.counter);
    pthread_join(thread, NULL);
    return shared.counter;
}
