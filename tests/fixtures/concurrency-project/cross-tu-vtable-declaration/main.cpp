// SPDX-License-Identifier: Apache-2.0
// Constructs a Widget, which only declares its vtable and type information here: constant data
// the program never writes, so nothing about this unit changes once the program is known.
#include "widget.hpp"

#include <typeinfo>

int main()
{
    Widget widget;
    return widget.value() == 42 && typeid(widget) == typeid(Widget) ? 0 : 1;
}
