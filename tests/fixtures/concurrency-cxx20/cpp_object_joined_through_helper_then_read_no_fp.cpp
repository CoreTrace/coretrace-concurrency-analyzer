// SPDX-License-Identifier: Apache-2.0
// The owner stops the object through a free function that calls the joining method, then reads
// the field the thread wrote. The handle joined two calls down is the one the constructor started,
// as seen from each call site in turn.
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

static void stop(Counter& counter)
{
    counter.finish();
}

int main()
{
    Counter counter;
    stop(counter);
    return counter.value();
}
