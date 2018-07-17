//===-- Highlighter.cpp -----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/Highlighter.h"

#include "lldb/Utility/AnsiTerminal.h"
#include "lldb/Utility/StreamString.h"

#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"

#include <unordered_set>

using namespace lldb_private;

std::size_t HighlightStyle::ColorStyle::Apply(Stream &s,
                                              llvm::StringRef value) const {
  // If we have no prefix, skip the expensive call below.
  if (!m_prefix.empty())
    s << lldb_utility::ansi::FormatAnsiTerminalCodes(m_prefix);
  s << value;
  if (!m_suffix.empty())
    s << lldb_utility::ansi::FormatAnsiTerminalCodes(m_suffix);
  // Calculate how many bytes we have written.
  return m_prefix.size() + value.size() + m_suffix.size();
}

std::size_t NoHighlighter::Highlight(const HighlightStyle &options,
                                     llvm::StringRef line,
                                     llvm::StringRef previous_lines,
                                     Stream &s) const {
  // We do nothing here.
  s << line;
  return line.size();
}

static HighlightStyle::ColorStyle GetColor(const char *c) {
  return HighlightStyle::ColorStyle(c, "${ansi.normal}");
}

HighlightStyle HighlightStyle::MakeVimStyle() {
  HighlightStyle result;
  result.comment = GetColor("${ansi.fg.purple}");
  result.scalar_literal = GetColor("${ansi.fg.red}");
  result.keyword = GetColor("${ansi.fg.green}");
  return result;
}

class ClangHighlighter : public Highlighter {
  llvm::StringSet<> keywords;
  /// Returns true if the given string represents a keywords in any Clang
  /// supported language.
  bool isKeyword(llvm::StringRef token) const;

  /// Determines which style should be applied to the given token.
  /// \param token
  ///     The current token.
  /// \param tok_str
  ///     The string in the source code the token represents.
  /// \param options
  ///     The style we use for coloring the source code.
  /// \param in_pp_directive
  ///     If we are currently in a preprocessor directive. NOTE: This is
  ///     passed by reference and will be updated if the current token starts
  ///     or ends a preprocessor directive.
  /// \return
  ///     The ColorStyle that should be applied to the token.
  HighlightStyle::ColorStyle determineClangStyle(const clang::Token &token,
                                                 llvm::StringRef tok_str,
                                                 const HighlightStyle &options,
                                                 bool &in_pp_directive) const;

public:
  ClangHighlighter();
  llvm::StringRef GetName() const override { return "clang"; }

  std::size_t Highlight(const HighlightStyle &options, llvm::StringRef line,
                        llvm::StringRef previous_lines,
                        Stream &s) const override;
  bool ShouldHighlightFile(lldb::LanguageType language,
                           llvm::StringRef path) const override;
};

bool ClangHighlighter::isKeyword(llvm::StringRef token) const {
  return keywords.find(token) != keywords.end();
}

ClangHighlighter::ClangHighlighter() {
#define KEYWORD(X, N) keywords.insert(#X);
#include "clang/Basic/TokenKinds.def"
}

HighlightStyle::ColorStyle ClangHighlighter::determineClangStyle(
    const clang::Token &token, llvm::StringRef tok_str,
    const HighlightStyle &options, bool &in_pp_directive) const {
  using namespace clang;

  if (token.is(tok::comment)) {
    // If we were in a preprocessor directive before, we now left it.
    in_pp_directive = false;
    return options.comment;
  } else if (in_pp_directive || token.getKind() == tok::hash) {
    // Let's assume that the rest of the line is a PP directive.
    in_pp_directive = true;
    // Preprocessor directives are hard to match, so we have to hack this in.
    return options.pp_directive;
  } else if (tok::isStringLiteral(token.getKind()))
    return options.string_literal;
  else if (tok::isLiteral(token.getKind()))
    return options.scalar_literal;
  else if (isKeyword(tok_str))
    return options.keyword;
  else
    switch (token.getKind()) {
    case tok::raw_identifier:
    case tok::identifier:
      return options.identifier;
    case tok::l_brace:
    case tok::r_brace:
      return options.braces;
    case tok::l_square:
    case tok::r_square:
      return options.square_brackets;
    case tok::l_paren:
    case tok::r_paren:
      return options.parentheses;
    case tok::comma:
      return options.comma;
    case tok::coloncolon:
    case tok::colon:
      return options.colon;

    case tok::amp:
    case tok::ampamp:
    case tok::ampequal:
    case tok::star:
    case tok::starequal:
    case tok::plus:
    case tok::plusplus:
    case tok::plusequal:
    case tok::minus:
    case tok::arrow:
    case tok::minusminus:
    case tok::minusequal:
    case tok::tilde:
    case tok::exclaim:
    case tok::exclaimequal:
    case tok::slash:
    case tok::slashequal:
    case tok::percent:
    case tok::percentequal:
    case tok::less:
    case tok::lessless:
    case tok::lessequal:
    case tok::lesslessequal:
    case tok::spaceship:
    case tok::greater:
    case tok::greatergreater:
    case tok::greaterequal:
    case tok::greatergreaterequal:
    case tok::caret:
    case tok::caretequal:
    case tok::pipe:
    case tok::pipepipe:
    case tok::pipeequal:
    case tok::question:
    case tok::equal:
    case tok::equalequal:
      return options.operators;
    default:
      break;
    }
  return HighlightStyle::ColorStyle();
}

std::size_t ClangHighlighter::Highlight(const HighlightStyle &options,
                                        llvm::StringRef line,
                                        llvm::StringRef previous_lines,
                                        Stream &result) const {
  using namespace clang;

  std::size_t written_bytes = 0;

  FileSystemOptions file_opts;
  FileManager file_mgr(file_opts);

  unsigned line_number = previous_lines.count('\n') + 1U;

  // Let's build the actual source code Clang needs and setup some utility
  // objects.
  std::string full_source = previous_lines.str() + line.str();
  llvm::IntrusiveRefCntPtr<DiagnosticIDs> diag_ids(new DiagnosticIDs());
  llvm::IntrusiveRefCntPtr<DiagnosticOptions> diags_opts(
      new DiagnosticOptions());
  DiagnosticsEngine diags(diag_ids, diags_opts);
  clang::SourceManager SM(diags, file_mgr);
  std::unique_ptr<llvm::MemoryBuffer> buf =
      llvm::MemoryBuffer::getMemBufferCopy(full_source);

  FileID FID = SM.createFileID(clang::SourceManager::Unowned, buf.get());

  // Let's just enable the latest ObjC and C++ which should get most tokens
  // right.
  LangOptions Opts;
  Opts.ObjC2 = true;
  Opts.CPlusPlus17 = true;
  Opts.LineComment = true;

  Lexer lex(FID, buf.get(), SM, Opts);
  // The lexer should keep whitespace around.
  lex.SetKeepWhitespaceMode(true);

  // Keeps track if we have entered a PP directive.
  bool in_pp_directive = false;

  // True once we actually lexed the user provided line.
  bool found_user_line = false;

  Token token;
  bool exit = false;
  while (!exit) {
    // Returns true if this is the last token we get from the lexer.
    exit = lex.LexFromRawLexer(token);

    bool invalid = false;
    unsigned current_line_number =
        SM.getSpellingLineNumber(token.getLocation(), &invalid);
    if (current_line_number != line_number)
      continue;
    found_user_line = true;

    // We don't need to print any tokens without a spelling line number.
    if (invalid)
      continue;

    // Same as above but with the column number.
    invalid = false;
    unsigned start = SM.getSpellingColumnNumber(token.getLocation(), &invalid);
    if (invalid)
      continue;
    // Column numbers start at 1, but indexes in our string start at 0.
    --start;

    // Annotations don't have a length, so let's skip them.
    if (token.isAnnotation())
      continue;

    // Extract the token string from our source code.
    llvm::StringRef tok_str = line.substr(start, token.getLength());

    // If the token is just an empty string, we can skip all the work below.
    if (tok_str.empty())
      continue;

    // See how we are supposed to highlight this token.
    HighlightStyle::ColorStyle color =
        determineClangStyle(token, tok_str, options, in_pp_directive);

    written_bytes += color.Apply(result, tok_str);
  }

  // If we went over the whole file but couldn't find our own file, then
  // somehow our setup was wrong. When we're in release mode we just give the
  // user the normal line and pretend we don't know how to highlight it. In
  // debug mode we bail out with an assert as this should never happen.
  if (!found_user_line) {
    result << line;
    written_bytes += line.size();
    assert(false && "We couldn't find the user line in the input file?");
  }

  return written_bytes;
}

bool ClangHighlighter::ShouldHighlightFile(lldb::LanguageType language,
                                           llvm::StringRef path) const {
  switch (language) {
  case lldb::LanguageType::eLanguageTypeC:
  case lldb::LanguageType::eLanguageTypeObjC:
  case lldb::LanguageType::eLanguageTypeObjC_plus_plus:
  case lldb::LanguageType::eLanguageTypeC89:
  case lldb::LanguageType::eLanguageTypeC99:
  case lldb::LanguageType::eLanguageTypeC11:
  case lldb::LanguageType::eLanguageTypeC_plus_plus:
  case lldb::LanguageType::eLanguageTypeC_plus_plus_03:
  case lldb::LanguageType::eLanguageTypeC_plus_plus_11:
  case lldb::LanguageType::eLanguageTypeC_plus_plus_14:
  case lldb::LanguageType::eLanguageTypeOpenCL:
    return true;
  default:
    break;
  }

  // User didn't provide any language, so we have to guess based on the file
  // path.
  const auto suffixes = {".cpp", ".cxx", ".c++", ".cc",  ".c",
                         ".h",   ".hh",  ".hpp", ".hxx", ".h++"};
  for (auto suffix : suffixes) {
    if (path.endswith_lower(suffix))
      return true;
  }

  // One final effort to check if we're in the STL path and should highlight.
  return path.contains("/c++/");
}

HighlighterManager::HighlighterManager() {
  m_highlighters.push_back(llvm::make_unique<ClangHighlighter>());

  // Our final highlighter will always match and just do nothing.
  m_highlighters.push_back(llvm::make_unique<NoHighlighter>());
}

const Highlighter &
HighlighterManager::getHighlighterFor(lldb::LanguageType language,
                                      llvm::StringRef path) const {
  for (auto &h : m_highlighters) {
    if (h->ShouldHighlightFile(language, path))
      return *h;
  }
  llvm_unreachable("No highlighter initialized?");
}

std::string Highlighter::Highlight(const HighlightStyle &options,
                                   llvm::StringRef line,
                                   llvm::StringRef previous_lines) const {
  StreamString s;
  Highlight(options, line, previous_lines, s);
  s.Flush();
  return s.GetString().str();
}
