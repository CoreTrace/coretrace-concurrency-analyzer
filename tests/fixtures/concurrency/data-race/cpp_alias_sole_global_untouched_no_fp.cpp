// SPDX-License-Identifier: Apache-2.0
// Regression: an access the walk cannot resolve — an element of a function-local vector,
// reached through a pointer the workers were handed — was attributed to `untouched_flag`
// merely because it is the only global in the module. Nothing ever reads or writes that
// global; a may-alias answer against a lone candidate carries no information.
#include <pthread.h>

#include <cstddef>
#include <vector>

int untouched_flag = 0;

struct Slot
{
    std::vector<char>* flags;
    std::size_t index;
};

static void* mark(void* raw)
{
    Slot* slot = static_cast<Slot*>(raw);
    (*slot->flags)[slot->index] = 1;
    return nullptr;
}

int main()
{
    std::vector<char> flags(2, 0);
    Slot first{&flags, 0};
    Slot second{&flags, 1};
    pthread_t firstWorker;
    pthread_t secondWorker;

    pthread_create(&firstWorker, nullptr, mark, &first);
    pthread_create(&secondWorker, nullptr, mark, &second);
    pthread_join(firstWorker, nullptr);
    pthread_join(secondWorker, nullptr);
    return flags[0] + flags[1];
}
