// SPDX-License-Identifier: Apache-2.0
// A method starts the thread on one branch; stop() joins it whenever it was started: main's
// write before stop() races, its read after does not (#113).
// Expected: one data race.
#include <thread>
static int hits;
class Service
{
  public:
    void start(bool wanted)
    {
        if (wanted)
            _thread = std::thread(&Service::run, this);
    }
    void stop()
    {
        if (_thread.joinable())
            _thread.join();
    }

  private:
    void run()
    {
        hits = hits + 1;
    }
    std::thread _thread;
};
int main(int argc, char**)
{
    Service service;
    service.start(argc > 1);
    hits = 5;
    service.stop();
    return hits;
}
