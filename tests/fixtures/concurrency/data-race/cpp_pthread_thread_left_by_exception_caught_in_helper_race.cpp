// SPDX-License-Identifier: Apache-2.0
// run_first starts first and joins it unless may_throw throws; guarded catches the exception and
// returns normally, first still running, and main starts second: the call to guarded does not
// end what it started (#224).
// Expected: one data race, first against second.
#include <pthread.h>
#include <stdexcept>

static int shared;
extern bool fail;

static void* first(void*)
{
    shared += 1;
    return nullptr;
}

static void* second(void*)
{
    shared += 2;
    return nullptr;
}

static void may_throw()
{
    if (fail)
        throw std::runtime_error("fail");
}

static void run_first()
{
    pthread_t a;
    pthread_create(&a, nullptr, first, nullptr);
    may_throw();
    pthread_join(a, nullptr);
}

static void guarded()
{
    try
    {
        run_first();
    }
    catch (const std::exception&)
    {
    }
}

bool fail = true;

int main()
{
    guarded();
    pthread_t b;
    pthread_create(&b, nullptr, second, nullptr);
    pthread_join(b, nullptr);
    return shared;
}
