// Driver unit: it owns the spawns and never sees the body they run.
#include <pthread.h>
#include <stddef.h>

void* worker(void* argument);

int main(void)
{
    pthread_t first;
    pthread_t second;

    pthread_create(&first, NULL, worker, NULL);
    pthread_create(&second, NULL, worker, NULL);

    pthread_join(first, NULL);
    pthread_join(second, NULL);
    return 0;
}
