// SPDX-License-Identifier: Apache-2.0
// The same std::for_each instantiation detaches the threads of one vector, then joins those of
// another: the first vector's reader still runs when main writes, the second's has ended.
// Expected: one data race, main against detachedReader.
#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

static int shared;

static void detachedReader()
{
    int seen = shared;
    (void)seen;
}

static void joinedReader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::vector<std::thread> detached;
    detached.emplace_back(std::thread(detachedReader));
    std::vector<std::thread> joined;
    joined.emplace_back(std::thread(joinedReader));
    std::for_each(detached.begin(), detached.end(), std::mem_fn(&std::thread::detach));
    std::for_each(joined.begin(), joined.end(), std::mem_fn(&std::thread::join));
    shared = 1;
    return shared;
}
