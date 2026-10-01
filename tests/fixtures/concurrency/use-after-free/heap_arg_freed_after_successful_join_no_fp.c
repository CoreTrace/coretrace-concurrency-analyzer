// SPDX-License-Identifier: Apache-2.0
// `data` is freed only on the branch where pthread_join succeeded: the worker has finished with
// it there.
// Expected: no diagnostic.
#include <pthread.h>
#include <stdlib.h>
static int sink;
static pthread_t thread;
static void* worker(void* argument)
{
    sink = *(int*)argument;
    return NULL;
}
int main(void)
{
    int* data = malloc(sizeof *data);
    *data = 1;
    pthread_create(&thread, NULL, worker, data);
    if (pthread_join(thread, NULL) == 0)
    {
        free(data);
        return sink;
    }
    return 1;
}
