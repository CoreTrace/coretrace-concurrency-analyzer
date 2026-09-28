// SPDX-License-Identifier: Apache-2.0
// The destructor reads `value` through a helper before it joins, while the thread may still be
// writing it. The helper's read is part of what the destructor does to the object, so the race
// stays reported.
#include <cstdio>
#include <thread>

class Counter
{
  public:
    Counter() : _worker(&Counter::run, this)
    {
    }

    ~Counter()
    {
        report();
        _worker.join();
    }

  private:
    void run()
    {
        _value = _value + 1;
    }

    void report() const
    {
        std::printf("%d\n", _value);
    }

    int _value = 0;
    std::thread _worker;
};

int main()
{
    Counter counter;
    return 0;
}
