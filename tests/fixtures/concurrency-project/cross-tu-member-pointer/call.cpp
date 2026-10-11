// SPDX-License-Identifier: Apache-2.0
// Calls the member function a pointer-to-member names on the thread it is handed: whether that
// joins the thread depends on the member each caller hands it.
#include <thread>

void call(std::thread& thread, void (std::thread::*member)())
{
    (thread.*member)();
}
