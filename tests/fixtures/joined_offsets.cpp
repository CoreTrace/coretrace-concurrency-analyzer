// SPDX-License-Identifier: Apache-2.0
// Functions joining threads inside the objects their parameters point to, for the offsets their
// join summaries record (#195). The file is compiled, never run.
#include <thread>

struct Pair
{
    std::thread first;
    std::thread second;
};

struct Nested
{
    long tag;
    Pair pair;
};

extern "C" void joinBoth(Pair* pair)
{
    pair->first.join();
    pair->second.join();
}

extern "C" void joinSecond(Pair* pair)
{
    pair->second.join();
}

extern "C" void joinSecondThroughHelper(Pair* pair)
{
    joinSecond(pair);
}

extern "C" void joinSecondOfInner(Nested* nested)
{
    joinSecond(&nested->pair);
}

// Calls itself on the next thread on every path: its summary gains an offset each round.
extern "C" void joinFrom(std::thread* thread)
{
    thread->join();
    joinFrom(thread + 1);
}
