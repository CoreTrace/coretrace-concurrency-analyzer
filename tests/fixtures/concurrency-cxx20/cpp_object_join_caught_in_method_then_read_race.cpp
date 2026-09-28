// SPDX-License-Identifier: Apache-2.0
// The method swallows the exception the join may throw and returns normally. On that path the
// join has waited for nothing, so the read after the method may race with the thread.
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
        try
        {
            _worker.join();
        }
        catch (const std::system_error&)
        {
        }
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
    counter.finish();
    return counter.value();
}
