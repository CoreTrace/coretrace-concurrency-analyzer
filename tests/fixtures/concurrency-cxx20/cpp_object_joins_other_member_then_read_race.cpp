// SPDX-License-Identifier: Apache-2.0
// The object runs two threads and the method joins only the one that does not write the field it
// then reads. Joining one member ends that thread, not the other.
#include <thread>

class Counter
{
  public:
    Counter() : _worker(&Counter::run, this), _idler(&Counter::idle, this)
    {
    }

    ~Counter()
    {
        if (_worker.joinable())
            _worker.join();
        if (_idler.joinable())
            _idler.join();
    }

    int finish()
    {
        _idler.join();
        return _value;
    }

  private:
    void run()
    {
        _value = _value + 1;
    }

    void idle()
    {
    }

    int _value = 0;
    std::thread _worker;
    std::thread _idler;
};

int main()
{
    Counter counter;
    return counter.finish();
}
