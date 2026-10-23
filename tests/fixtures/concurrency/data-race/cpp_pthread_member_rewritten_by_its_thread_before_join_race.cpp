// SPDX-License-Identifier: Apache-2.0
// The reader is handed the object holding its own handle, and replaces that handle with idle's
// before main joins it through a member function called by pointer: the join waits for idle, and
// the reader still reads shared when main writes it. A thread handed the storage may rewrite the
// handle, so the member function's join is not trusted (#213).
// Expected: three data races and one missing join. The race on shared is the one this pins. The
// two on the handle are conservative, the atomics ordering the rewrite before main reads it, and
// so is the missing join of idle, which is joined through the rewritten handle.
#include <atomic>
#include <pthread.h>

static int shared;
static pthread_t idle;
static std::atomic<bool> published(false), changed(false);

struct Worker
{
    pthread_t thread;

    void finish()
    {
        pthread_join(thread, nullptr);
    }
};

static void* reader(void* argument)
{
    auto* worker = static_cast<Worker*>(argument);
    while (!published.load(std::memory_order_acquire))
    {
    }
    worker->thread = idle;
    changed.store(true, std::memory_order_release);
    int seen = shared;
    (void)seen;
    return nullptr;
}

static void* nothing(void*)
{
    return nullptr;
}

static void call(Worker& worker, void (Worker::*member)())
{
    (worker.*member)();
}

int main()
{
    pthread_create(&idle, nullptr, nothing, nullptr);
    Worker worker;
    pthread_create(&worker.thread, nullptr, reader, &worker);
    pthread_t original = worker.thread;
    published.store(true, std::memory_order_release);
    while (!changed.load(std::memory_order_acquire))
    {
    }
    call(worker, &Worker::finish);
    shared = 1;
    pthread_join(original, nullptr);
    return shared;
}
