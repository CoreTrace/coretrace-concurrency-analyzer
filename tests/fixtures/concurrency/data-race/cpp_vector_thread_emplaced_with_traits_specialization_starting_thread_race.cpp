// SPDX-License-Identifier: Apache-2.0
// A specialization of std::allocator_traits for the vector's allocator, which the standard allows
// before C++23, starts a detached thread in construct. Its code lies in namespace std, yet it is
// the program's: joining the element does not end that thread, which still reads when main
// writes (#123).
// Expected: one data race, main against background.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <pthread.h>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

static int shared;

static void* background(void*)
{
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(shared));
}

static void worker() {}

template <class T> struct Alloc
{
    using value_type = T;
    Alloc() = default;
    template <class U> Alloc(const Alloc<U>&) {}
    T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
    void deallocate(T* p, std::size_t n) { std::allocator<T>{}.deallocate(p, n); }
    template <class U> bool operator==(const Alloc<U>&) const { return true; }
};

namespace std
{
    template <class T> struct allocator_traits<Alloc<T>>
    {
        using allocator_type = Alloc<T>;
        using value_type = T;
        using pointer = T*;
        using const_pointer = const T*;
        using void_pointer = void*;
        using const_void_pointer = const void*;
        using difference_type = std::ptrdiff_t;
        using size_type = std::size_t;
        using propagate_on_container_copy_assignment = std::false_type;
        using propagate_on_container_move_assignment = std::true_type;
        using propagate_on_container_swap = std::false_type;
        using is_always_equal = std::true_type;
        template <class U> using rebind_alloc = Alloc<U>;
        template <class U> using rebind_traits = allocator_traits<Alloc<U>>;
        static pointer allocate(allocator_type& a, size_type n) { return a.allocate(n); }
        static void deallocate(allocator_type& a, pointer p, size_type n) { a.deallocate(p, n); }
        template <class U, class... Args> static void construct(allocator_type&, U* p, Args&&... args)
        {
            // Only building the element from its entry starts one: relocating moves threads.
            if constexpr (!(std::is_same_v<std::decay_t<Args>, U> || ...))
            {
                pthread_t extra;
                pthread_create(&extra, nullptr, background, nullptr);
                pthread_detach(extra);
            }
            std::construct_at(p, std::forward<Args>(args)...);
        }
        template <class U> static void destroy(allocator_type&, U* p) { p->~U(); }
        static size_type max_size(const allocator_type&) noexcept { return std::size_t(-1) / sizeof(T); }
        static allocator_type select_on_container_copy_construction(const allocator_type& a)
        {
            return a;
        }
    };
} // namespace std

int main()
{
    std::vector<std::thread, Alloc<std::thread>> threads;
    threads.emplace_back(worker);
    threads.back().join();
    shared += 1;
    return shared;
}
