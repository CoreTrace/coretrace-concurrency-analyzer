// Creation unit: it starts the worker and never joins it. On its own that is an unjoined
// handle, and saying so is right — this unit has no way to know what happens next.
#include <pthread.h>
#include <stddef.h>

pthread_t g_worker;

static void* run(void* argument)
{
    (void)argument;
    return NULL;
}

void start_worker(void)
{
    pthread_create(&g_worker, NULL, run, NULL);
}
