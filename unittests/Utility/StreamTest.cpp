//===-- StreamTest.cpp ------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Utility/StreamString.h"
#include "gtest/gtest.h"

using namespace lldb_private;

namespace {
struct StreamTest : ::testing::Test {
  // Note: Stream is an abstract class, so we use StreamString to test it. To
  // make it easier to change this later, only methods in this class explicitly
  // refer to the StringStream class.
  StreamString s;
  // We return here a std::string because that way gtest can print better
  // assertion messages.
  std::string Value() const {
    return s.GetString().str();
  }
};
}

namespace {
// A StreamTest where we expect the Stream output to be binary.
struct BinaryStreamTest : StreamTest {
  void SetUp() override {
    s.GetFlags().Set(Stream::eBinary);
  }
};
}

TEST_F(StreamTest, ChangingByteOrder) {
  s.SetByteOrder(lldb::eByteOrderPDP);
  EXPECT_EQ(lldb::eByteOrderPDP, s.GetByteOrder());
}

TEST_F(StreamTest, PutChar) {
  s.PutChar('a');
  EXPECT_EQ("a", Value());

  s.PutChar('1');
  EXPECT_EQ("a1", Value());
}

TEST_F(StreamTest, PutCharWhitespace) {
  s.PutChar(' ');
  EXPECT_EQ(" ", Value());

  s.PutChar('\n');
  EXPECT_EQ(" \n", Value());

  s.PutChar('\r');
  EXPECT_EQ(" \n\r", Value());

  s.PutChar('\t');
  EXPECT_EQ(" \n\r\t", Value());
}

TEST_F(StreamTest, PutCString) {
  s.PutCString("");
  EXPECT_EQ("", Value());

  s.PutCString("foobar");
  EXPECT_EQ("foobar", Value());

  s.PutCString(" ");
  EXPECT_EQ("foobar ", Value());
}

TEST_F(StreamTest, PutCStringWithStringRef) {
  s.PutCString(llvm::StringRef(""));
  EXPECT_EQ("", Value());

  s.PutCString(llvm::StringRef("foobar"));
  EXPECT_EQ("foobar", Value());

  s.PutCString(llvm::StringRef(" "));
  EXPECT_EQ("foobar ", Value());
}

TEST_F(StreamTest, QuotedCString) {
  s.QuotedCString("foo");
  EXPECT_EQ("\"foo\"", Value());

  s.QuotedCString("bar");
  EXPECT_EQ("\"foo\"\"bar\"", Value());

  s.QuotedCString(" ");
  EXPECT_EQ("\"foo\"\"bar\"\" \"", Value());
}

TEST_F(StreamTest, PutCharNull) {
  s.PutChar('\0');
  EXPECT_EQ(std::string("\0", 1), Value());

  s.PutChar('a');
  EXPECT_EQ(std::string("\0a", 2), Value());
}

TEST_F(StreamTest, PutCStringAsRawHex8) {
  s.PutCStringAsRawHex8("");
  // FIXME: Check that printing 00 on an empty string is the intended behavior.
  // It seems kind of unexpected  that we print the trailing 0 byte for empty
  // strings, but not for non-empty strings.
  EXPECT_EQ("00", Value());

  s.PutCStringAsRawHex8("foobar");
  EXPECT_EQ("00666f6f626172", Value());

  s.PutCStringAsRawHex8(" ");
  EXPECT_EQ("00666f6f62617220", Value());
}

TEST_F(StreamTest, PutHex8) {
  s.PutHex8((uint8_t)55);
  EXPECT_EQ("37", Value());
  s.PutHex8(std::numeric_limits<uint8_t>::max());
  EXPECT_EQ("37ff", Value());
  s.PutHex8((uint8_t)0);
  EXPECT_EQ("37ff00", Value());
}

TEST_F(StreamTest, PutNHex8) {
  s.PutNHex8(0, (uint8_t)55);
  EXPECT_EQ("", Value());
  s.PutNHex8(1, (uint8_t)55);
  EXPECT_EQ("37", Value());
  s.PutNHex8(2, (uint8_t)55);
  EXPECT_EQ("373737", Value());
  s.PutNHex8(1, (uint8_t)56);
  EXPECT_EQ("37373738", Value());
}

TEST_F(StreamTest, PutHex16ByteOrderLittle) {
  s.PutHex16(0x1234U, lldb::eByteOrderLittle);
  EXPECT_EQ("3412", Value());
  s.PutHex16(std::numeric_limits<uint16_t>::max(), lldb::eByteOrderLittle);
  EXPECT_EQ("3412ffff", Value());
  s.PutHex16(0U, lldb::eByteOrderLittle);
  EXPECT_EQ("3412ffff0000", Value());
}

TEST_F(StreamTest, PutHex16ByteOrderBig) {
  s.PutHex16(0x1234U, lldb::eByteOrderBig);
  EXPECT_EQ("1234", Value());
  s.PutHex16(std::numeric_limits<uint16_t>::max(), lldb::eByteOrderBig);
  EXPECT_EQ("1234ffff", Value());
  s.PutHex16(0U, lldb::eByteOrderBig);
  EXPECT_EQ("1234ffff0000", Value());
}

TEST_F(StreamTest, PutHex32ByteOrderLittle) {
  s.PutHex32(0x12345678U, lldb::eByteOrderLittle);
  EXPECT_EQ("78563412", Value());
  s.PutHex32(std::numeric_limits<uint32_t>::max(), lldb::eByteOrderLittle);
  EXPECT_EQ("78563412ffffffff", Value());
  s.PutHex32(0U, lldb::eByteOrderLittle);
  EXPECT_EQ("78563412ffffffff00000000", Value());
}

TEST_F(StreamTest, PutHex32ByteOrderBig) {
  s.PutHex32(0x12345678U, lldb::eByteOrderBig);
  EXPECT_EQ("12345678", Value());
  s.PutHex32(std::numeric_limits<uint32_t>::max(), lldb::eByteOrderBig);
  EXPECT_EQ("12345678ffffffff", Value());
  s.PutHex32(0U, lldb::eByteOrderBig);
  EXPECT_EQ("12345678ffffffff00000000", Value());
}

TEST_F(StreamTest, PutHex64ByteOrderLittle) {
  s.PutHex64(0x1234567890ABCDEFU, lldb::eByteOrderLittle);
  EXPECT_EQ("efcdab9078563412", Value());
  s.PutHex64(std::numeric_limits<uint64_t>::max(), lldb::eByteOrderLittle);
  EXPECT_EQ("efcdab9078563412ffffffffffffffff", Value());
  s.PutHex64(0U, lldb::eByteOrderLittle);
  EXPECT_EQ("efcdab9078563412ffffffffffffffff0000000000000000", Value());
}

TEST_F(StreamTest, PutHex64ByteOrderBig) {
  s.PutHex64(0x1234567890ABCDEFU, lldb::eByteOrderBig);
  EXPECT_EQ("1234567890abcdef", Value());
  s.PutHex64(std::numeric_limits<uint64_t>::max(), lldb::eByteOrderBig);
  EXPECT_EQ("1234567890abcdefffffffffffffffff", Value());
  s.PutHex64(0U, lldb::eByteOrderBig);
  EXPECT_EQ("1234567890abcdefffffffffffffffff0000000000000000", Value());
}

//------------------------------------------------------------------------------
// Shift operator tests.
//------------------------------------------------------------------------------

TEST_F(StreamTest, ShiftOperatorChars) {
  s << 'a' << 'b';
  EXPECT_EQ("ab", Value());
}

TEST_F(StreamTest, ShiftOperatorStrings) {
  s << "cstring\n";
  s << llvm::StringRef("llvm::StringRef\n");
  EXPECT_EQ("cstring\nllvm::StringRef\n", Value());
}

TEST_F(StreamTest, ShiftOperatorInts) {
  s << std::numeric_limits<int8_t>::max() << " ";
  s << std::numeric_limits<int16_t>::max() << " ";
  s << std::numeric_limits<int32_t>::max() << " ";
  s << std::numeric_limits<int64_t>::max();
  EXPECT_EQ("127 32767 2147483647 9223372036854775807", Value());
}

TEST_F(StreamTest, ShiftOperatorUInts) {
  s << std::numeric_limits<uint8_t>::max() << " ";
  s << std::numeric_limits<uint16_t>::max() << " ";
  s << std::numeric_limits<uint32_t>::max() << " ";
  s << std::numeric_limits<uint64_t>::max();
  EXPECT_EQ("ff ffff ffffffff ffffffffffffffff", Value());
}

TEST_F(StreamTest, ShiftOperatorPtr) {
  // This test is a bit tricky because pretty much everything related to
  // pointer printing seems to lead to UB or IB. So let's make the most basic
  // test that just checks that we print *something*. This way we at least know
  // that pointer printing doesn't do really bad things (e.g. crashing, reading
  // OOB/uninitialized memory which the sanitizers would spot).

  // Shift our own pointer to the output.
  int i = 3;
  int *ptr = &i;
  s << ptr;

  EXPECT_TRUE(!Value().empty());
}

TEST_F(StreamTest, PutPtr) {
  // See the ShiftOperatorPtr test for the rationale.
  int i = 3;
  int *ptr = &i;
  s.PutPointer(ptr);

  EXPECT_TRUE(!Value().empty());
}

// Alias to make it more clear that 'invalid' means for the Stream interface
// that it should use the host byte order.
const static auto hostByteOrder = lldb::eByteOrderInvalid;

//------------------------------------------------------------------------------
// PutRawBytes/PutBytesAsRawHex tests.
//------------------------------------------------------------------------------

TEST_F(StreamTest, PutBytesAsRawHex8ToBigEndian) {
  uint32_t value = 0x12345678;
  s.PutBytesAsRawHex8(static_cast<void*>(&value), sizeof(value),
                      hostByteOrder, lldb::eByteOrderBig);
  EXPECT_EQ("78563412", Value());
}

TEST_F(StreamTest, PutRawBytesToBigEndian) {
  uint32_t value = 0x12345678;
  s.PutRawBytes(static_cast<void*>(&value), sizeof(value),
                      hostByteOrder, lldb::eByteOrderBig);
  EXPECT_EQ("\x78\x56\x34\x12", Value());
}

TEST_F(StreamTest, PutBytesAsRawHex8ToLittleEndian) {
  uint32_t value = 0x12345678;
  s.PutBytesAsRawHex8(static_cast<void*>(&value), sizeof(value),
                      hostByteOrder, lldb::eByteOrderLittle);
  EXPECT_EQ("12345678", Value());
}

TEST_F(StreamTest, PutRawBytesToLittleEndian) {
  uint32_t value = 0x12345678;
  s.PutRawBytes(static_cast<void*>(&value), sizeof(value),
                      hostByteOrder, lldb::eByteOrderLittle);
  EXPECT_EQ("\x12\x34\x56\x78", Value());
}

TEST_F(StreamTest, PutBytesAsRawHex8ToMixedEndian) {
  uint32_t value = 0x12345678;
  s.PutBytesAsRawHex8(static_cast<void*>(&value), sizeof(value),
                      hostByteOrder, lldb::eByteOrderPDP);

  // FIXME: PDP byte order is not actually implemented but Stream just silently
  // prints the value in some random byte order...
#if 0
  EXPECT_EQ("34127856", Value());
#endif
}

TEST_F(StreamTest, PutRawBytesToMixedEndian) {
  uint32_t value = 0x12345678;
  s.PutRawBytes(static_cast<void*>(&value), sizeof(value),
                      lldb::eByteOrderInvalid, lldb::eByteOrderPDP);

  // FIXME: PDP byte order is not actually implemented but Stream just silently
  // prints the value in some random byte order...
#if 0
  EXPECT_EQ("\x34\x12\x78\x56", Value());
#endif
}

//------------------------------------------------------------------------------
// ULEB128 support for binary streams.
//------------------------------------------------------------------------------

TEST_F(BinaryStreamTest, PutULEB128OneByte) {
  auto bytes = s.PutULEB128(0x74ULL);
  EXPECT_EQ("\x74", Value());
  EXPECT_EQ(1U, bytes);
}

TEST_F(BinaryStreamTest, PutULEB128TwoBytes) {
  auto bytes = s.PutULEB128(0x1985ULL);
  EXPECT_EQ("\x85\x33", Value());
  EXPECT_EQ(2U, bytes);
}

TEST_F(BinaryStreamTest, PutULEB128ThreeBytes) {
  auto bytes = s.PutULEB128(0x5023ULL);
  EXPECT_EQ("\xA3\xA0\x1", Value());
  EXPECT_EQ(3U, bytes);
}

TEST_F(BinaryStreamTest, PutULEB128FourBytes) {
  auto bytes = s.PutULEB128(0xA48032ULL);
  EXPECT_EQ("\xB2\x80\x92\x5", Value());
  EXPECT_EQ(4U, bytes);
}

TEST_F(BinaryStreamTest, PutULEB128FiveBytes) {
  auto bytes = s.PutULEB128(0x12345678ULL);
  EXPECT_EQ("\xF8\xAC\xD1\x91\x1", Value());
  EXPECT_EQ(5U, bytes);
}

TEST_F(BinaryStreamTest, PutULEB128SixBytes) {
  auto bytes = s.PutULEB128(0xABFE3FAFDFULL);
  EXPECT_EQ("\xDF\xDF\xFE\xF1\xBF\x15", Value());
  EXPECT_EQ(6U, bytes);
}

TEST_F(BinaryStreamTest, PutULEB128SevenBytes) {
  auto bytes = s.PutULEB128(0xDABFE3FAFDFULL);
  EXPECT_EQ("\xDF\xDF\xFE\xF1\xBF\xB5\x3", Value());
  EXPECT_EQ(7U, bytes);
}

TEST_F(BinaryStreamTest, PutULEB128EightBytes) {
  auto bytes = s.PutULEB128(0x7CDABFE3FAFDFULL);
  EXPECT_EQ("\xDF\xDF\xFE\xF1\xBF\xB5\xF3\x3", Value());
  EXPECT_EQ(8U, bytes);
}

TEST_F(BinaryStreamTest, PutULEB128NineBytes) {
  auto bytes = s.PutULEB128(0x327CDABFE3FAFDFULL);
  EXPECT_EQ("\xDF\xDF\xFE\xF1\xBF\xB5\xF3\x93\x3", Value());
  EXPECT_EQ(9U, bytes);
}

TEST_F(BinaryStreamTest, PutULEB128MaxValue) {
  auto bytes = s.PutULEB128(std::numeric_limits<uint64_t>::max());
  EXPECT_EQ("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x1", Value());
  EXPECT_EQ(10U, bytes);
}

TEST_F(BinaryStreamTest, PutULEB128Zero) {
  auto bytes = s.PutULEB128(0x0U);
  EXPECT_EQ(std::string("\0", 1), Value());
  EXPECT_EQ(1U, bytes);
}

TEST_F(BinaryStreamTest, PutULEB128One) {
  auto bytes = s.PutULEB128(0x1U);
  EXPECT_EQ("\x1", Value());
  EXPECT_EQ(1U, bytes);
}

//------------------------------------------------------------------------------
// SLEB128 support for binary streams.
//------------------------------------------------------------------------------

TEST_F(BinaryStreamTest, PutSLEB128OneByte) {
  auto bytes = s.PutSLEB128(0x74LL);
  EXPECT_EQ(std::string("\xF4\0", 2), Value());
  EXPECT_EQ(2U, bytes);
}

TEST_F(BinaryStreamTest, PutSLEB128TwoBytes) {
  auto bytes = s.PutSLEB128(0x1985LL);
  EXPECT_EQ("\x85\x33", Value());
  EXPECT_EQ(2U, bytes);
}

TEST_F(BinaryStreamTest, PutSLEB128ThreeBytes) {
  auto bytes = s.PutSLEB128(0x5023LL);
  EXPECT_EQ("\xA3\xA0\x1", Value());
  EXPECT_EQ(3U, bytes);
}

TEST_F(BinaryStreamTest, PutSLEB128FourBytes) {
  auto bytes = s.PutSLEB128(0xA48032LL);
  EXPECT_EQ("\xB2\x80\x92\x5", Value());
  EXPECT_EQ(4U, bytes);
}

TEST_F(BinaryStreamTest, PutSLEB128FiveBytes) {
  auto bytes = s.PutSLEB128(0x12345678LL);
  EXPECT_EQ("\xF8\xAC\xD1\x91\x1", Value());
  EXPECT_EQ(5U, bytes);
}

TEST_F(BinaryStreamTest, PutSLEB128SixBytes) {
  auto bytes = s.PutSLEB128(0xABFE3FAFDFLL);
  EXPECT_EQ("\xDF\xDF\xFE\xF1\xBF\x15", Value());
  EXPECT_EQ(6U, bytes);
}

TEST_F(BinaryStreamTest, PutSLEB128SevenBytes) {
  auto bytes = s.PutSLEB128(0xDABFE3FAFDFLL);
  EXPECT_EQ("\xDF\xDF\xFE\xF1\xBF\xB5\x3", Value());
  EXPECT_EQ(7U, bytes);
}

TEST_F(BinaryStreamTest, PutSLEB128EightBytes) {
  auto bytes = s.PutSLEB128(0x7CDABFE3FAFDFLL);
  EXPECT_EQ("\xDF\xDF\xFE\xF1\xBF\xB5\xF3\x3", Value());
  EXPECT_EQ(8U, bytes);
}

TEST_F(BinaryStreamTest, PutSLEB128NineBytes) {
  auto bytes = s.PutSLEB128(0x327CDABFE3FAFDFLL);
  EXPECT_EQ("\xDF\xDF\xFE\xF1\xBF\xB5\xF3\x93\x3", Value());
  EXPECT_EQ(9U, bytes);
}

TEST_F(BinaryStreamTest, PutSLEB128MaxValue) {
  auto bytes = s.PutSLEB128(std::numeric_limits<int64_t>::max());
  EXPECT_EQ(std::string("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\0", 10), Value());
  EXPECT_EQ(10U, bytes);
}

TEST_F(BinaryStreamTest, PutSLEB128Zero) {
  auto bytes = s.PutSLEB128(0x0);
  EXPECT_EQ(std::string("\0", 1), Value());
  EXPECT_EQ(1U, bytes);
}

TEST_F(BinaryStreamTest, PutSLEB128One) {
  auto bytes = s.PutSLEB128(0x1);
  EXPECT_EQ(std::string("\x1", 1), Value());
  EXPECT_EQ(1U, bytes);
}

//------------------------------------------------------------------------------
// SLEB128/ULEB128 support for non-binary streams.
//------------------------------------------------------------------------------

// The logic for this is very simple, so it should be enough to test some basic
// use cases.

TEST_F(StreamTest, PutULEB128) {
  auto bytes = s.PutULEB128(0x74ULL);
  EXPECT_EQ("0x74", Value());
  EXPECT_EQ(4U, bytes);
}

TEST_F(StreamTest, PutSLEB128) {
  auto bytes = s.PutSLEB128(0x1985LL);
  EXPECT_EQ("0x6533", Value());
  EXPECT_EQ(6U, bytes);
}
