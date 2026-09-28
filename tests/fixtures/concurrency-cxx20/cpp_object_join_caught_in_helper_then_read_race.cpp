// SPDX-License-Identifier: Apache-2.0
// The helper calls the joining method but swallows the exception it may throw, and returns
// normally either way. On that path nothing waited for the thread, so the read after the helper
// may race with it.
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

static void stopQuietly(Counter& counter)
{
    try
    {
        counter.finish();
    }
    catch (const std::system_error&)
    {
    }
}

int main()
{
    Counter counter;
    stopQuietly(counter);
    return counter.value();
}
