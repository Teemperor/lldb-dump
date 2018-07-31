//===-- EndianTest.cpp ------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "lldb/Utility/Endian.h"

using namespace lldb_private;

TEST(EndianTest, HostByteOrder) {
  switch((unsigned)endian::InlHostByteOrder()) {
  case lldb::eByteOrderBig:
  case lldb::eByteOrderPDP:
  case lldb::eByteOrderLittle:
    SUCCEED();
    break;
  case lldb::eByteOrderInvalid:
    FAIL() << "Host byte order is invalid.";
    break;
  default:
    FAIL() << "Unknown host byte order enum value.";
    break;
  }
}
