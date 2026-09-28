// SPDX-License-Identifier: Apache-2.0
// The owner detaches the object's thread, then hands the object another thread, made elsewhere,
// which the object joins. That join ends the thread handed over; the detached one is still running
// when the owner reads.
#include <thread>
#include <utility>

static void idle()
{
}

static std::thread makeIdleThread()
{
    return std::thread(idle);
}

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

    void release()
    {
        _worker.detach();
    }

    void adopt(std::thread worker)
    {
        _worker = std::move(worker);
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
    counter.release();
    counter.adopt(makeIdleThread());
    counter.finish();
    return counter.value();
}
