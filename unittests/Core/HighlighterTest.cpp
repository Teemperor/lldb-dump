//===-- HighlighterTest.cpp -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "lldb/Core/Highlighter.h"

using namespace lldb_private;
using namespace llvm;

static std::string getName(lldb::LanguageType type) {
  HighlighterManager mgr;
  return mgr.getHighlighterFor(type, "").GetName().str();
}

static std::string getName(llvm::StringRef path) {
  HighlighterManager mgr;
  return mgr.getHighlighterFor(lldb::eLanguageTypeUnknown, path)
      .GetName()
      .str();
}

TEST(HighlighterTest, HighlighterSelectionType) {
  ASSERT_EQ(getName(lldb::eLanguageTypeC), "clang");
  ASSERT_EQ(getName(lldb::eLanguageTypeC11), "clang");
  ASSERT_EQ(getName(lldb::eLanguageTypeC89), "clang");
  ASSERT_EQ(getName(lldb::eLanguageTypeC99), "clang");
  ASSERT_EQ(getName(lldb::eLanguageTypeC_plus_plus), "clang");
  ASSERT_EQ(getName(lldb::eLanguageTypeC_plus_plus_03), "clang");
  ASSERT_EQ(getName(lldb::eLanguageTypeC_plus_plus_11), "clang");
  ASSERT_EQ(getName(lldb::eLanguageTypeC_plus_plus_14), "clang");
  ASSERT_EQ(getName(lldb::eLanguageTypeObjC), "clang");
  ASSERT_EQ(getName(lldb::eLanguageTypeObjC_plus_plus), "clang");
  ASSERT_EQ(getName(lldb::eLanguageTypeOpenCL), "clang");

  ASSERT_NE(getName(lldb::eLanguageTypeJulia), "clang");
  ASSERT_NE(getName(lldb::eLanguageTypeJava), "clang");
  ASSERT_NE(getName(lldb::eLanguageTypeHaskell), "clang");
}

TEST(HighlighterTest, HighlighterSelectionPath) {
  ASSERT_EQ(getName("myfile.cc"), "clang");
  ASSERT_EQ(getName("moo.cpp"), "clang");
  ASSERT_EQ(getName("mar.cxx"), "clang");
  ASSERT_EQ(getName("foo.C"), "clang");
  ASSERT_EQ(getName("bar.CC"), "clang");
  ASSERT_EQ(getName("a/dir.CC"), "clang");
  ASSERT_EQ(getName("/a/dir.hpp"), "clang");
  ASSERT_EQ(getName("header.h"), "clang");

  ASSERT_NE(getName("/dev/null"), "clang");
  ASSERT_NE(getName("Factory.java"), "clang");
  ASSERT_NE(getName("poll.py"), "clang");
  ASSERT_NE(getName("reducer.hs"), "clang");
}

TEST(HighlighterTest, FallbackHighlighter) {
  HighlighterManager mgr;
  const Highlighter &h =
      mgr.getHighlighterFor(lldb::eLanguageTypePascal83, "foo.pas");

  HighlightStyle style;
  style.identifier.Set("[", "]");
  style.semicolons.Set("<", ">");

  const char *code = "program Hello;";
  std::string output = h.Highlight(style, code);

  ASSERT_STREQ(output.c_str(), code);
}

TEST(HighlighterTest, DefaultHighlighter) {
  HighlighterManager mgr;
  const Highlighter &h = mgr.getHighlighterFor(lldb::eLanguageTypeC, "main.c");

  HighlightStyle style;

  const char *code = "int my_main() { return 22; } \n";
  std::string output = h.Highlight(style, code);

  ASSERT_STREQ(output.c_str(), code);
}

//------------------------------------------------------------------------------
// Tests highlighting with the Clang highlighter.
//------------------------------------------------------------------------------

static std::string highlightC(llvm::StringRef code, HighlightStyle style) {
  HighlighterManager mgr;
  const Highlighter &h = mgr.getHighlighterFor(lldb::eLanguageTypeC, "main.c");
  return h.Highlight(style, code);
}

TEST(HighlighterTest, ClangEmptyInput) {
  HighlightStyle s;

  std::string output = highlightC("", s);
  ASSERT_STREQ("", output.c_str());
}

TEST(HighlighterTest, ClangScalarLiterals) {
  HighlightStyle s;
  s.scalar_literal.Set("<scalar>", "</scalar>");

  std::string output = highlightC(" int i = 22;", s);
  ASSERT_STREQ(" int i = <scalar>22</scalar>;", output.c_str());
}

TEST(HighlighterTest, ClangStringLiterals) {
  HighlightStyle s;
  s.string_literal.Set("<str>", "</str>");

  std::string output = highlightC("const char *f = 22 + \"foo\";", s);
  ASSERT_STREQ("const char *f = 22 + <str>\"foo\"</str>;", output.c_str());
}

TEST(HighlighterTest, ClangUnterminatedString) {
  HighlightStyle s;
  s.string_literal.Set("<str>", "</str>");

  std::string output = highlightC(" f = \"", s);
  ASSERT_STREQ(" f = \"", output.c_str());
}

TEST(HighlighterTest, Keywords) {
  HighlightStyle s;
  s.keyword.Set("<k>", "</k>");

  std::string output = highlightC(" return 1; ", s);
  ASSERT_STREQ(" <k>return</k> 1; ", output.c_str());
}

TEST(HighlighterTest, Colons) {
  HighlightStyle s;
  s.colon.Set("<c>", "</c>");

  std::string output = highlightC("foo::bar:", s);
  ASSERT_STREQ("foo<c>:</c><c>:</c>bar<c>:</c>", output.c_str());
}

TEST(HighlighterTest, ClangBraces) {
  HighlightStyle s;
  s.braces.Set("<b>", "</b>");

  std::string output = highlightC("a{}", s);
  ASSERT_STREQ("a<b>{</b><b>}</b>", output.c_str());
}

TEST(HighlighterTest, ClangSquareBrackets) {
  HighlightStyle s;
  s.square_brackets.Set("<sb>", "</sb>");

  std::string output = highlightC("a[]", s);
  ASSERT_STREQ("a<sb>[</sb><sb>]</sb>", output.c_str());
}

TEST(HighlighterTest, ClangCommas) {
  HighlightStyle s;
  s.comma.Set("<comma>", "</comma>");

  std::string output = highlightC(" bool f = foo(), 1;", s);
  ASSERT_STREQ(" bool f = foo()<comma>,</comma> 1;", output.c_str());
}

TEST(HighlighterTest, ClangPPDirectives) {
  HighlightStyle s;
  s.pp_directive.Set("<pp>", "</pp>");

  std::string output = highlightC(" #include \"foo\" // comment\n", s);
  ASSERT_STREQ(" <pp>#</pp><pp>include</pp><pp> </pp><pp>\"foo\"</pp><pp> </pp>"
               "// comment\n",
               output.c_str());
}

TEST(HighlighterTest, ClangComments) {
  HighlightStyle s;
  s.comment.Set("<cc>", "</cc>");

  std::string output = highlightC(" /*com */ // com /*n*/", s);
  ASSERT_STREQ(" <cc>/*com */</cc> <cc>// com /*n*/</cc>", output.c_str());
}

TEST(HighlighterTest, ClangOperators) {
  HighlightStyle s;
  s.operators.Set("[", "]");

  std::string output = highlightC(" 1+2/a*f&x|~l", s);
  ASSERT_STREQ(" 1[+]2[/]a[*]f[&]x[|][~]l", output.c_str());
}

TEST(HighlighterTest, ClangIdentifiers) {
  HighlightStyle s;
  s.identifier.Set("<id>", "</id>");

  std::string output = highlightC(" foo c = bar(); return 1;", s);
  ASSERT_STREQ(" <id>foo</id> <id>c</id> = <id>bar</id>(); return 1;",
               output.c_str());
}
