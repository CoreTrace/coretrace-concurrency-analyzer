// SPDX-License-Identifier: Apache-2.0
// The object starts its own thread in its constructor, so nothing in main looks like a spawn.
// The thread is running all the same from the moment the constructor returns, and the method the
// owner calls next races with it.
#include <thread>

class Counter
{
  public:
    Counter() : _worker(&Counter::run, this)
    {
    }

    ~Counter()
    {
        _worker.join();
    }

    void touch()
    {
        _value = _value + 1;
    }

  private:
    void run()
    {
        _value = _value + 1;
    }

    int _value = 0;
    std::thread _worker;
};

int main()
{
    Counter counter;
    counter.touch();
    return 0;
}
