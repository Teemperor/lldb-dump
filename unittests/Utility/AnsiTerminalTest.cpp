//===-- AnsiTerminalTest.cpp ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "lldb/Utility/AnsiTerminal.h"

using namespace lldb_utility;

TEST(AnsiTerminal, Empty) {
  std::string format = ansi::FormatAnsiTerminalCodes("");
  EXPECT_STREQ("", format.c_str());
}

TEST(AnsiTerminal, WhiteSpace) {
  std::string format = ansi::FormatAnsiTerminalCodes(" ");
  EXPECT_STREQ(" ", format.c_str());
}

TEST(AnsiTerminal, AtEnd) {
  std::string format = ansi::FormatAnsiTerminalCodes("abc${ansi.fg.black}");
  EXPECT_STREQ("abc\x1B[30m", format.c_str());
}

TEST(AnsiTerminal, AtStart) {
  std::string format = ansi::FormatAnsiTerminalCodes("${ansi.fg.black}abc");
  EXPECT_STREQ("\x1B[30mabc", format.c_str());
}

TEST(AnsiTerminal, KnownPrefix) {
  std::string format = ansi::FormatAnsiTerminalCodes("${ansi.fg.redish}abc");
  EXPECT_STREQ("${ansi.fg.redish}abc", format.c_str());
}

TEST(AnsiTerminal, Unknown) {
  std::string format = ansi::FormatAnsiTerminalCodes("${ansi.fg.foo}abc");
  EXPECT_STREQ("${ansi.fg.foo}abc", format.c_str());
}

TEST(AnsiTerminal, Incomplete) {
  std::string format = ansi::FormatAnsiTerminalCodes("abc${ansi.");
  EXPECT_STREQ("abc${ansi.", format.c_str());
}

TEST(AnsiTerminal, Twice) {
  std::string format =
      ansi::FormatAnsiTerminalCodes("${ansi.fg.black}${ansi.fg.red}abc");
  EXPECT_STREQ("\x1B[30m\x1B[31mabc", format.c_str());
}

TEST(AnsiTerminal, Basic) {
  std::string format =
      ansi::FormatAnsiTerminalCodes("abc${ansi.fg.red}abc${ansi.normal}abc");
  EXPECT_STREQ("abc\x1B[31mabc\x1B[0mabc", format.c_str());
}
