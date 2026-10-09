// SPDX-License-Identifier: Apache-2.0
// The pointer-to-member names a virtual member, whose target is whatever the object's dynamic
// type overrides it with: here it detaches the thread, which still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

struct Stopper
{
    virtual ~Stopper() = default;
    virtual void stop(std::thread& thread)
    {
        thread.detach();
    }
};

int main()
{
    std::thread thread(reader);
    Stopper stopper;
    void (Stopper::*member)(std::thread&) = &Stopper::stop;
    (stopper.*member)(thread);
    shared = 1;
    return shared;
}
