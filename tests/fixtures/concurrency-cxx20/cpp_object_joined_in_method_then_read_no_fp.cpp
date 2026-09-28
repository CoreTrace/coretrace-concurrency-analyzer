// SPDX-License-Identifier: Apache-2.0
// The object's constructor starts its thread and a later method joins it before reading the field
// the thread writes. The join in the method ends the thread the constructor started: the read
// comes after it.
#include <thread>

class Counter
{
  public:
    Counter() : _worker(&Counter::run, this)
    {
    }

    ~Counter()
    {
        if (_worker.joinable())
            _worker.join();
    }

    int finish()
    {
        _worker.join();
        return _value;
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
    return counter.finish();
}
