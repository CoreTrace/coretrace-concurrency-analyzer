// SPDX-License-Identifier: Apache-2.0
// A helper hands the field it is given to a function whose body is not in the unit. What that
// function may do covers the object it is given, which is that field and not the whole global:
// the worker's write to another field does not overlap it.
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
    hand_over(&shared.flag);
    pthread_join(thread, NULL);
    return shared.counter;
}
