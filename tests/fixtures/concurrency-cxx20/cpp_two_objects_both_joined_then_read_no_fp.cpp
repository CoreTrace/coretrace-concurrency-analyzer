// SPDX-License-Identifier: Apache-2.0
// The control for cpp_two_objects_one_joined_then_read_race.cpp: both objects' threads are joined
// before the global is read.
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
    second.finish();
    return hits;
}
