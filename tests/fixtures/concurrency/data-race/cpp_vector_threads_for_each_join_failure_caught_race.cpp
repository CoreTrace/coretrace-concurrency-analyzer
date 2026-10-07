// SPDX-License-Identifier: Apache-2.0
// The lambda std::for_each applies catches the exception a join may throw and goes on: a thread
// whose join failed may still run when main writes.
// Expected: one data race, main against reader.
#include <algorithm>
#include <system_error>
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    std::for_each(threads.begin(), threads.end(),
                  [](std::thread& thread)
                  {
                      try
                      {
                          thread.join();
                      }
                      catch (const std::system_error&)
                      {
                      }
                  });
    shared = 1;
    return shared;
}
