// SPDX-License-Identifier: Apache-2.0
// The control for cpp_object_joined_in_method_then_read_no_fp.cpp: the method detaches the thread
// instead of joining it, so the thread may still be writing when the method reads.
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
        _worker.detach();
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
