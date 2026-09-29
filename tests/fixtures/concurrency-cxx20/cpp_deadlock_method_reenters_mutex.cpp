// SPDX-License-Identifier: Apache-2.0
// add() holds the object's std::mutex and calls count(), which locks it again through `this`: the
// thread waits for a mutex it holds itself. Both the worker and main call add().
// Expected: two deadlocks, one reacquisition at each call to add().
#include <mutex>
#include <thread>

class Registry
{
  public:
    void add(int value)
    {
        std::lock_guard<std::mutex> guard(lock_);
        total_ += value;
        count();
    }

    int count()
    {
        std::lock_guard<std::mutex> guard(lock_);
        return ++calls_;
    }

  private:
    std::mutex lock_;
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
