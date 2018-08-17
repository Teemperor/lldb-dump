//===-- IOHandlerStackTest.cpp ----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/IOHandler.h"
#include "gtest/gtest.h"

using namespace lldb_private;

struct TestHandler : public IOHandler {
  long m_cancel_count = 0;

  void Run() override {}

  void Cancel() override {}

  bool Interrupt() override { return false; }

  void GotEOF() override {}
};

static std::pair<TestHandler *, IOHandlerSP> MakeTestHandler() {
  TestHandler *handler = new TestHandler();
  std::shared_ptr handler_sp(handler);
  return std::make_pair(handler, handler_sp);
}

TEST(IOHandlerStackTest, IsActive) {
  IOHandlerStack stack;
  auto handler = MakeTestHandler();

  stack.Push(handler.second);
  EXPECT_TRUE(handler.first->IsActive());
  stack.Pop();
  EXPECT_FALSE(handler.first->IsActive());
}

TEST(IOHandlerStackTest, IsActiveNested) {
  IOHandlerStack stack;
  auto handler1 = MakeTestHandler();
  auto handler2 = MakeTestHandler();

  stack.Push(handler1.second);
  EXPECT_TRUE(handler1.first->IsActive());
  EXPECT_FALSE(handler2.first->IsActive());

  stack.Push(handler2.second);
  EXPECT_FALSE(handler1.first->IsActive());
  EXPECT_TRUE(handler2.first->IsActive());

  stack.Pop();
  EXPECT_TRUE(handler1.first->IsActive());
  EXPECT_FALSE(handler2.first->IsActive());

  stack.Pop();
  EXPECT_FALSE(handler1.first->IsActive());
  EXPECT_FALSE(handler2.first->IsActive());
}
