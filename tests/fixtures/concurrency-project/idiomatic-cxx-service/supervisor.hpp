// SPDX-License-Identifier: Apache-2.0
#ifndef IDIOMATIC_CXX_SERVICE_SUPERVISOR_HPP_
#define IDIOMATIC_CXX_SERVICE_SUPERVISOR_HPP_

#include <cstddef>

/// Starts child processes that keep running the same program, the way a service spreads work
/// across cores without an executable of its own to exec into.
void spawn_children(std::size_t count);

#endif
