// SPDX-License-Identifier: Apache-2.0
// Defines Widget's key function, so this unit holds its vtable and type information.
#include "widget.hpp"

Widget::~Widget() = default;

int Widget::value() const
{
    return 42;
}
