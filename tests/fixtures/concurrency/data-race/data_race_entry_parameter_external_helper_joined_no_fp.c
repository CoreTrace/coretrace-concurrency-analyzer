// SPDX-License-Identifier: Apache-2.0
// A helper other units may call starts the entry it is handed, giving that thread nothing of the
// storage its handle lies in: whatever entry other calls hand it, main's join ends the thread
// (#201, #235).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int shared;
static pthread_t thread;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

void start(void* (*entry)(void*));

void start(void* (*entry)(void*))
{
    pthread_create(&thread, NULL, entry, NULL);
}

int main(void)
{
    start(worker);
    pthread_join(thread, NULL);
    return shared;
}
