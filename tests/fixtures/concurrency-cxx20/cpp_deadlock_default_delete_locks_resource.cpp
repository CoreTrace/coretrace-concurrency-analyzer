// SPDX-License-Identifier: Apache-2.0
// Releasing a resource takes its own lock, then the registry lock, then the pool lock: the deleter
// std::unique_ptr calls is a specialization of std::default_delete. The auditor takes the pool
// lock, then the registry lock, while the worker releases its resource. No call takes over the
// orders of a function of namespace std, so the deleter's order stands for its calls, though a lock
// of the object it is handed is held around it (#136).
// Expected: one deadlock.
#include <memory>
#include <mutex>
#include <thread>

struct Resource
{
    std::mutex mutex;
    int value = 0;
};

std::mutex registryMutex;
std::mutex poolMutex;

namespace std
{
    template <>
    struct default_delete<Resource>
    {
        void operator()(Resource* resource) const
        {
            {
                std::lock_guard<std::mutex> own(resource->mutex);
                std::lock_guard<std::mutex> registry(registryMutex);
                std::lock_guard<std::mutex> pool(poolMutex);
            }
            delete resource;
        }
    };
} // namespace std

void worker()
{
    std::unique_ptr<Resource> resource(new Resource);
    resource.reset();
}

void auditor()
{
    std::lock_guard<std::mutex> pool(poolMutex);
    std::lock_guard<std::mutex> registry(registryMutex);
}

int main()
{
    std::thread releasing(worker);
    std::thread auditing(auditor);
    releasing.join();
    auditing.join();
    return 0;
}
