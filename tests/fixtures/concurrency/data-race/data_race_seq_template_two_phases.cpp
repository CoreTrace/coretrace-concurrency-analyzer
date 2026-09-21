// SPDX-License-Identifier: Apache-2.0
// Cause (a) in the exact shape of AnalyzerApp.cpp::runParallelWork: each call site instantiates
// its own helper whose trampoline lambda is a distinct thread entry, and the join lives in the
// helper. Expected today: a race pairing the two `__invoke` entries, although they never coexist.
#include <pthread.h>
int g_shared = 0;
template <typename Fn>
static void runOnce(Fn&& fn) {
    struct Ctx { Fn* fn; };
    Ctx ctx{&fn};
    pthread_t th;
    pthread_create(&th, nullptr,
        [](void* raw) -> void* { (*static_cast<Ctx*>(raw)->fn)(); return nullptr; }, &ctx);
    pthread_join(th, nullptr);
}
int main() {
    runOnce([] { g_shared++; });
    runOnce([] { g_shared++; });
    return g_shared;
}
