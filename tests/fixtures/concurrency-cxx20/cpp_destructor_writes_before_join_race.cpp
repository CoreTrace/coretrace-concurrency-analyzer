// SPDX-License-Identifier: Apache-2.0
// The control for cpp_object_joins_its_thread_in_destructor_no_fp.cpp: the destructor resets
// `value` before it joins, while the thread may still be writing it. The race is real.
#include <thread>

class Counter
{
  public:
    Counter() : _worker(&Counter::run, this)
    {
    }

    ~Counter()
    {
        _value = 0;
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
