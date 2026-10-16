// SPDX-License-Identifier: Apache-2.0
// Each thread is moved into a slot of a member array and every slot is joined by a second loop
// before main writes (#206).
// Expected: no diagnostic.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

struct Holder
{
    std::thread threads[2];
};

int main()
{
    Holder holder;
    for (int i = 0; i < 2; ++i)
        holder.threads[i] = std::thread(reader);
    for (int i = 0; i < 2; ++i)
        holder.threads[i].join();
    shared = 1;
    return shared;
}
