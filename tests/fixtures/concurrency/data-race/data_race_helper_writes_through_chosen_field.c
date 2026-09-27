// SPDX-License-Identifier: Apache-2.0
// A helper writes through a pointer it picks at run time among two fields of the object it is
// handed. Which field it writes is unknown, so what the call may do covers both, and the race
// with the worker's write to one of them stays reported.
#include <pthread.h>
#include <stddef.h>

struct Pair
{
    int first;
    int second;
};

static struct Pair pair;

static void set_one(struct Pair* target, int pick_second)
{
    int* slot = pick_second ? &target->second : &target->first;
    *slot = 1;
}

static void* worker(void* argument)
{
    (void)argument;
    pair.first += 1;
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    set_one(&pair, argc > 1);
    pthread_join(thread, NULL);
    return pair.first;
}
