// SPDX-License-Identifier: Apache-2.0
// The method joins the thread on one branch only, then reads the field the thread writes on both.
// On the branch that skips the join the thread may still be running.
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

    int finish(bool wait)
    {
        if (wait)
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

int main(int argc, char**)
{
    Counter counter;
    return counter.finish(argc > 1);
}
