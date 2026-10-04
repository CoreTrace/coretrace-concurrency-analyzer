// SPDX-License-Identifier: Apache-2.0
// main calls itself once before joining; both invocations hand &results[i] to a thread, so two
// threads write each element (#108).
// Expected: one data race.
#include <pthread.h>
static int results[4];
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
int main(int argc, char** argv)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    if (argc < 2)
        main(argc + 1, argv);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return 0;
}
