// SPDX-License-Identifier: Apache-2.0
// The pointer-to-member names Stopper's virtual stop, which joins, and is called on a Detacher:
// the call dispatches to Detacher's override, which detaches the thread instead. The reader still
// runs when main writes.
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
        thread.join();
    }
};

struct Detacher : Stopper
{
    void stop(std::thread& thread) override
    {
        thread.detach();
    }
};

static void stopWith(Stopper& stopper, std::thread& thread)
{
    void (Stopper::*member)(std::thread&) = &Stopper::stop;
    (stopper.*member)(thread);
}

int main()
{
    std::thread thread(reader);
    Detacher detacher;
    stopWith(detacher, thread);
    shared = 1;
    return shared;
}
