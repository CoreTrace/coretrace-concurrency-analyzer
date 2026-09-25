// SPDX-License-Identifier: Apache-2.0
#pragma once

// A polymorphic class whose key function, its first non-inline virtual member, is defined in
// widget.cpp. The compiler emits the vtable and the type information there, and every other
// unit that constructs a Widget only declares them.
class Widget
{
  public:
    Widget() = default;
    virtual ~Widget();
    virtual int value() const;
};
