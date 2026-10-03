// SPDX-License-Identifier: Apache-2.0
// The thread is joined on both branches of an if, each going on only past its join's success.
// main's write before the if runs beside the worker; its write after the if, on another
// variable, comes after the thread certainly ended on every path (#113, L3).
// Expected: one data race, on before_value.
#include <pthread.h>
#include <stddef.h>

static int before_value;
static int after_value;

static void* worker(void* arg)
{
    (void)arg;
    before_value = 1;
    after_value = 1;
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    before_value = 2;
    if (argc > 1)
    {
        if (pthread_join(thread, NULL) != 0)
            return 1;
    }
    else
    {
        if (pthread_join(thread, NULL) != 0)
            return 2;
    }
    after_value = 2;
    return 0;
}
