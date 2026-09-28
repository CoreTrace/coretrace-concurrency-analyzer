// SPDX-License-Identifier: Apache-2.0
// The owner reads the field the thread writes in the handler of an exception the join may throw.
// A join that throws has not waited for anything, so on that path the thread may still be running.
#include <system_error>
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

    void finish()
    {
        _worker.join();
    }

    int value() const
    {
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
    try
    {
        counter.finish();
    }
    catch (const std::system_error&)
    {
        return counter.value();
    }
    return 0;
}
