// Application unit: it spawns both threads and holds both conflicting accesses, but the object
// they fight over is only declared here. Alone, this unit cannot tell that `extern` from an
// unresolved symbol, so it stays silent.
#include <pthread.h>
#include <stddef.h>

extern int g_shared_state;

static void* reader(void* argument)
{
    (void)argument;
    volatile int observed = g_shared_state;
    (void)observed;
    return NULL;
}

static void* writer(void* argument)
{
    (void)argument;
    g_shared_state = 42;
    return NULL;
}

int main(void)
{
    pthread_t readerThread;
    pthread_t writerThread;

    pthread_create(&readerThread, NULL, reader, NULL);
    pthread_create(&writerThread, NULL, writer, NULL);

    pthread_join(readerThread, NULL);
    pthread_join(writerThread, NULL);
    return 0;
}
