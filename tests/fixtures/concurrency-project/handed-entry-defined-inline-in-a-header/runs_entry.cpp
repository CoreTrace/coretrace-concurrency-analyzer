// SPDX-License-Identifier: Apache-2.0
// Runs writer, which worker.hpp defines inline and main.cpp starts on main's own task, on an object
// of its own: under that object's lock, which excludes nothing reader holds. Using writer makes
// this unit define its own copy of it, without declaring it.
// Expected in project mode with main.cpp: one data race on shared, between reader and writer.
#include "worker.hpp"

void run_other()
{
    Task mine = {PTHREAD_MUTEX_INITIALIZER, {}};
    writer(&mine);
}
