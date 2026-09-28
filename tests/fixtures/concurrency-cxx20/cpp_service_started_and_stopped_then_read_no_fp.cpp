// SPDX-License-Identifier: Apache-2.0
// A service starts its thread in one method, moving it into a member, and joins that member in
// another. The handle keeps its identity through the move, so the read after `stop` comes after
// the thread has finished.
#include <thread>

class Service
{
  public:
    void start()
    {
        _worker = std::thread(&Service::run, this);
    }

    void stop()
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
    Service service;
    service.start();
    service.stop();
    return service.value();
}
