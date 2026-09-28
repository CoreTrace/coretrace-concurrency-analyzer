// SPDX-License-Identifier: Apache-2.0
// The same object, but the thread is assigned in the constructor body after `value` is set. The
// owner never touches `value` again before the join in the destructor.
#include <thread>

class Counter
{
  public:
    Counter() : _value(0)
    {
        _worker = std::thread(&Counter::run, this);
    }

    ~Counter()
    {
        _worker.join();
    }

  private:
    void run()
    {
        _value = _value + 1;
    }

    int _value;
    std::thread _worker;
};

int main()
{
    Counter counter;
    return 0;
}
