// SPDX-License-Identifier: Apache-2.0
// A helper writes ids[i] before thread i starts; thread i reads only ids[i]; main writes the next
// id while earlier threads run (#108).
// Expected: no diagnostic.
#include <pthread.h>
static void set_id(int* target, int value)
{
    *target = value;
}
static void* worker(void* argument)
{
    volatile int id = *(int*)argument;
    (void)id;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    int ids[4];
    for (int i = 0; i < 4; ++i)
    {
        set_id(&ids[i], i);
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return 0;
}
