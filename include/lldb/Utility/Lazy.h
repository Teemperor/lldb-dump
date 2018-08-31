//===-- Lazy.h --------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_Lazy_h_
#define liblldb_Lazy_h_

#include "lldb/lldb-private-enumerations.h"

namespace lldb_private {
template <typename T, typename Class, T (Class::*Update)()> class LazyMember {
  T m_value;
  bool m_needs_update;

public:
  LazyMember() { reset(); }

  void reset() { m_needs_update = true; }

  const T &get(Class &instance) {
    if (m_needs_update)
      set((instance.*Update)());
    return m_value;
  }

  void set(const T &value) {
    m_value = value;
    m_needs_update = false;
  }
};

template <typename Class, bool (Class::*Update)()>
class LazyBoolMember : public LazyMember<bool, Class, Update> {};
} // namespace lldb_private

#endif // liblldb_Lazy_h_
