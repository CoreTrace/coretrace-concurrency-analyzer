// SPDX-License-Identifier: Apache-2.0
// Two objects each start a thread that bumps a global. Joining the first one's thread leaves the
// second one's running, so the read of the global still races with it.
#include <thread>

static int hits = 0;

class Worker
{
  public:
    Worker() : _thread(&Worker::run, this)
    {
    }

    ~Worker()
    {
        if (_thread.joinable())
            _thread.join();
    }

    void finish()
    {
        _thread.join();
    }

  private:
    void run()
    {
        hits = hits + 1;
    }

    std::thread _thread;
};

int main()
{
    Worker first;
    Worker second;
    first.finish();
    return hits;
}
