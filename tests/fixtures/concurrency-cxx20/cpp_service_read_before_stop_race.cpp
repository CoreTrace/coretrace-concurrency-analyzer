// SPDX-License-Identifier: Apache-2.0
// The control for cpp_service_started_and_stopped_then_read_no_fp.cpp: the owner reads before it
// stops the service, while the thread may still be writing.
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
    const int seen = service.value();
    service.stop();
    return seen;
}
