// SPDX-License-Identifier: Apache-2.0
// Regression: a captureless lambda passed to `pthread_create` is lowered to a call to the
// closure's conversion-to-function-pointer operator, whose only job is to return the
// `__invoke` thunk. The thread entry used to stay unresolved behind that call, so the race
// between the two workers on `shared_counter` was never reported.
#include <pthread.h>

int shared_counter = 0;

static void* worker(void*)
{
    for (int i = 0; i < 1000; ++i)
        shared_counter++;
    return nullptr;
}

int main()
{
    pthread_t firstWorker;
    pthread_t secondWorker;

    pthread_create(
        &firstWorker, nullptr, [](void* raw) -> void* { return worker(raw); }, nullptr);
    pthread_create(
        &secondWorker, nullptr, [](void* raw) -> void* { return worker(raw); }, nullptr);
    pthread_join(firstWorker, nullptr);
    pthread_join(secondWorker, nullptr);
    return shared_counter;
}
