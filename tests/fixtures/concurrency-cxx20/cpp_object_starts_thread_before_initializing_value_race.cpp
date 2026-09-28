// SPDX-License-Identifier: Apache-2.0
// The object declares its `std::thread` member before `_value`, and members are initialized in
// declaration order: the thread starts running `run` before `_value = 0` is stored, and that store
// races with it. The destructor joins the thread through the member, the object's first field. The
// join writes that member only, not the whole object, so it does not race with `run` (#110).
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

    std::thread _worker;
    int _value = 0;
};

int main()
{
    Counter counter;
    return 0;
}
