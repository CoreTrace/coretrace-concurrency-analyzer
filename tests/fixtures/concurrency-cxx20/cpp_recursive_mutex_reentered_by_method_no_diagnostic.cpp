// SPDX-License-Identifier: Apache-2.0
// add() holds the object's std::recursive_mutex and calls count(), which locks it again through
// `this`. A recursive mutex lets its owner take it again, and it is only ever reached through
// `this`, never by name.
// Expected: no diagnostic.
#include <mutex>
#include <thread>

class Registry
{
  public:
    void add(int value)
    {
        std::lock_guard<std::recursive_mutex> guard(lock_);
        total_ += value;
        count();
    }

    int count()
    {
        std::lock_guard<std::recursive_mutex> guard(lock_);
        return ++calls_;
    }

  private:
    std::recursive_mutex lock_;
    int total_ = 0;
    int calls_ = 0;
};

static Registry registry;

static void worker()
{
    registry.add(1);
}

int main()
{
    std::thread thread(worker);
    registry.add(2);
    thread.join();
    return registry.count();
}
