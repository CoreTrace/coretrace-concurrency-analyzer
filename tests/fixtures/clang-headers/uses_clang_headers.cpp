// SPDX-License-Identifier: Apache-2.0
// A unit of a Clang-based tool: it includes Clang's headers, which the image must provide.
#include <clang/AST/ASTConsumer.h>

int count_consumers(const clang::ASTConsumer* consumer)
{
    return consumer != nullptr ? 1 : 0;
}
