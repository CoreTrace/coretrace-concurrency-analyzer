// SPDX-License-Identifier: Apache-2.0
// pthread_once runs the initializer exactly once and makes every caller wait for it, so the
// readers see the configuration it wrote.
#include <pthread.h>
#include <stddef.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int config = 0;

static void init_config(void)
{
    config = 42;
}

static void* reader(void* argument)
{
    (void)argument;
    pthread_once(&once, init_config);
    return (void*)(long)config;
}

int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&threads[i], NULL, reader, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(threads[i], NULL);
    return 0;
}
