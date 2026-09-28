// SPDX-License-Identifier: Apache-2.0
// The object starts its thread in its member initializers and joins it in its destructor. The
// thread only touches `value`; the destructor only touches the `std::thread` member, through the
// call from the complete destructor to the base one. What that call may do to the object is
// what its callee does, not the whole object.
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
    return 0;
}
