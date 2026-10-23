// SPDX-License-Identifier: Apache-2.0
// As cpp_pthread_helpers_joining_through_pointer_in_loop_no_fp.cpp, the loop stopping one slot
// short: the last reader still runs when main writes, and main joins it after (#213).
// Expected: one data race, main against reader.
#include <pthread.h>

static int shared;

static void* reader(void*)
{
    int seen = shared;
    (void)seen;
    return nullptr;
}

static void stop(pthread_t* handle)
{
    pthread_join(*handle, nullptr);
}

int main()
{
    pthread_t threads[3];
    for (int i = 0; i < 3; ++i)
        pthread_create(&threads[i], nullptr, reader, nullptr);
    for (int i = 0; i < 2; ++i)
        stop(&threads[i]);
    shared = 1;
    pthread_join(threads[2], nullptr);
    return shared;
}
