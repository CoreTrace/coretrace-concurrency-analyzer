// SPDX-License-Identifier: Apache-2.0
// The control for data_race_helper_writes_array_element_no_fp.c: the helper is handed the element
// the worker writes, so the race is real and must stay reported.
#include <pthread.h>
#include <stddef.h>

static int values[8];

static void* worker(void* argument)
{
    (void)argument;
    values[2] += 1;
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
    return values[2];
}
