//===- Tokens.h - collect tokens from preprocessing --------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Record tokens that a preprocessor emits and define operations to map between
// the tokens written in a file and tokens produced by the preprocessor.
//
// When running the compiler, there are two token streams we are interested in:
//   - "spelled" tokens directly correspond to a substring written in some
//     source file.
//   - "expanded" tokens represent the result of preprocessing, parses consumes
//     this token stream to produce the AST.
//
// Expanded tokens correspond directly to locations found in the AST, allowing
// to find subranges of the token stream covered by various AST nodes. Spelled
// tokens correspond directly to the source code written by the user.
//
// To allow composing these two use-cases, we also define operations that map
// between expanded and spelled tokens that produced them (macro calls,
// directives, etc).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLING_SYNTAX_TOKENS_H
#define LLVM_CLANG_TOOLING_SYNTAX_TOKENS_H

#include "clang/Basic/FileManager.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Token.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <tuple>

namespace clang {
class Preprocessor;

namespace syntax {

/// A half-open character range inside a particular file, the start offset is
/// included and the end offset is excluded from the range.
struct FileRange {
  /// EXPECTS: File.isValid() && Begin <= End.
  FileRange(FileID File, unsigned BeginOffset, unsigned EndOffset);
  /// EXPECTS: BeginLoc.isValid() && BeginLoc.isFileID().
  FileRange(const SourceManager &SM, SourceLocation BeginLoc, unsigned Length);
  /// EXPECTS: BeginLoc.isValid() && BeginLoc.isFileID(), Begin <= End and files
  ///          are the same.
  FileRange(const SourceManager &SM, SourceLocation BeginLoc,
            SourceLocation EndLoc);

  FileID file() const { return File; }
  /// Start is a start offset (inclusive) in the corresponding file.
  unsigned beginOffset() const { return Begin; }
  /// End offset (exclusive) in the corresponding file.
  unsigned endOffset() const { return End; }

  unsigned length() const { return End - Begin; }

  /// Check if \p Offset is inside the range.
  bool contains(unsigned Offset) const {
    return Begin <= Offset && Offset < End;
  }
  /// Check \p Offset is inside the range or equal to its endpoint.
  bool touches(unsigned Offset) const {
    return Begin <= Offset && Offset <= End;
  }

  /// Gets the substring that this FileRange refers to.
  llvm::StringRef text(const SourceManager &SM) const;

  friend bool operator==(const FileRange &L, const FileRange &R) {
    return std::tie(L.File, L.Begin, L.End) == std::tie(R.File, R.Begin, R.End);
  }
  friend bool operator!=(const FileRange &L, const FileRange &R) {
    return !(L == R);
  }

private:
  FileID File;
  unsigned Begin;
  unsigned End;
};

/// For debugging purposes.
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const FileRange &R);

/// A token coming directly from a file or from a macro invocation. Has just
/// enough information to locate the token in the source code.
/// Can represent both expanded and spelled tokens.
class Token {
public:
  Token(SourceLocation Location, unsigned Length, tok::TokenKind Kind);
  /// EXPECTS: clang::Token is not an annotation token.
  explicit Token(const clang::Token &T);

  tok::TokenKind kind() const { return Kind; }
  /// Location of the first character of a token.
  SourceLocation location() const { return Location; }
  /// Location right after the last character of a token.
  SourceLocation endLocation() const {
    return Location.getLocWithOffset(Length);
  }
  unsigned length() const { return Length; }

  /// Get the substring covered by the token. Note that will include all
  /// digraphs, newline continuations, etc. E.g. tokens for 'int' and
  ///    in\
  ///    t
  /// both have the same kind tok::kw_int, but results of text() are different.
  llvm::StringRef text(const SourceManager &SM) const;

  /// Gets a range of this token.
  /// EXPECTS: token comes from a file, not from a macro expansion.
  FileRange range(const SourceManager &SM) const;

  /// Given two tokens inside the same file, returns a file range that starts at
  /// \p First and ends at \p Last.
  /// EXPECTS: First and Last are file tokens from the same file, Last starts
  ///          after First.
  static FileRange range(const SourceManager &SM, const syntax::Token &First,
                         const syntax::Token &Last);

  std::string dumpForTests(const SourceManager &SM) const;
  /// For debugging purposes.
  std::string str() const;

private:
  SourceLocation Location;
  unsigned Length;
  tok::TokenKind Kind;
};
/// For debugging purposes. Equivalent to a call to Token::str().
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Token &T);

/// A list of tokens obtained by preprocessing a text buffer and operations to
/// map between the expanded and spelled tokens, i.e. TokenBuffer has
/// information about two token streams:
///    1. Expanded tokens: tokens produced by the preprocessor after all macro
///       replacements,
///    2. Spelled tokens: corresponding directly to the source code of a file
///       before any macro replacements occurred.
/// Here's an example to illustrate a difference between those two:
///     #define FOO 10
///     int a = FOO;
///
/// Spelled tokens are {'#','define','FOO','10','int','a','=','FOO',';'}.
/// Expanded tokens are {'int','a','=','10',';','eof'}.
///
/// Note that the expanded token stream has a tok::eof token at the end, the
/// spelled tokens never store a 'eof' token.
///
/// The full list expanded tokens can be obtained with expandedTokens(). Spelled
/// tokens for each of the files can be obtained via spelledTokens(FileID).
///
/// To map between the expanded and spelled tokens use findSpelledByExpanded().
///
/// To build a token buffer use the TokenCollector class. You can also compute
/// the spelled tokens of a file using the tokenize() helper.
///
/// FIXME: allow to map from spelled to expanded tokens when use-case shows up.
/// FIXME: allow mappings into macro arguments.
class TokenBuffer {
public:
  TokenBuffer(const SourceManager &SourceMgr) : SourceMgr(&SourceMgr) {}
  /// All tokens produced by the preprocessor after all macro replacements,
  /// directives, etc. Source locations found in the clang AST will always
  /// point to one of these tokens.
  /// FIXME: figure out how to handle token splitting, e.g. '>>' can be split
  ///        into two '>' tokens by the parser. However, TokenBuffer currently
  ///        keeps it as a single '>>' token.
  llvm::ArrayRef<syntax::Token> expandedTokens() const {
    return ExpandedTokens;
  }

  /// Find the subrange of spelled tokens that produced the corresponding \p
  /// Expanded tokens.
  ///
  /// EXPECTS: \p Expanded is a subrange of expandedTokens().
  ///
  /// Will fail if the expanded tokens do not correspond to a
  /// sequence of spelled tokens. E.g. for the following example:
  ///
  ///   #define FIRST f1 f2 f3
  ///   #define SECOND s1 s2 s3
  ///
  ///   a FIRST b SECOND c // expanded tokens are: a f1 f2 f3 b s1 s2 s3 c
  ///
  /// the results would be:
  ///   expanded   => spelled
  ///   ------------------------
  ///            a => a
  ///     s1 s2 s3 => SECOND
  ///   a f1 f2 f3 => a FIRST
  ///         a f1 => can't map
  ///        s1 s2 => can't map
  ///
  /// If \p Expanded is empty, the returned value is llvm::None.
  /// Complexity is logarithmic.
  llvm::Optional<llvm::ArrayRef<syntax::Token>>
  spelledForExpanded(llvm::ArrayRef<syntax::Token> Expanded) const;

  /// An expansion produced by the preprocessor, includes macro expansions and
  /// preprocessor directives. Preprocessor always maps a non-empty range of
  /// spelled tokens to a (possibly empty) range of expanded tokens. Here is a
  /// few examples of expansions:
  ///    #pragma once      // Expands to an empty range.
  ///    #define FOO 1 2 3 // Expands an empty range.
  ///    FOO               // Expands to "1 2 3".
  /// FIXME(ibiryukov): implement this, currently #include expansions are empty.
  ///    #include <vector> // Expands to tokens produced by the include.
  struct Expansion {
    llvm::ArrayRef<syntax::Token> Spelled;
    llvm::ArrayRef<syntax::Token> Expanded;
  };
  /// If \p Spelled starts a mapping (e.g. if it's a macro name or '#' starting
  /// a preprocessor directive) return the subrange of expanded tokens that the
  /// macro expands to.
  llvm::Optional<Expansion>
  expansionStartingAt(const syntax::Token *Spelled) const;

  /// Lexed tokens of a file before preprocessing. E.g. for the following input
  ///     #define DECL(name) int name = 10
  ///     DECL(a);
  /// spelledTokens() returns {"#", "define", "DECL", "(", "name", ")", "eof"}.
  /// FIXME: we do not yet store tokens of directives, like #include, #define,
  ///        #pragma, etc.
  llvm::ArrayRef<syntax::Token> spelledTokens(FileID FID) const;

  /// Get all tokens that expand a macro in \p FID. For the following input
  ///     #define FOO B
  ///     #define FOO2(X) int X
  ///     FOO2(XY)
  ///     int B;
  ///     FOO;
  /// macroExpansions() returns {"FOO2", "FOO"} (from line 3 and 5
  /// respecitvely).
  std::vector<const syntax::Token *> macroExpansions(FileID FID) const;

  const SourceManager &sourceManager() const { return *SourceMgr; }

  std::string dumpForTests() const;

private:
  /// Describes a mapping between a continuous subrange of spelled tokens and
  /// expanded tokens. Represents macro expansions, preprocessor directives,
  /// conditionally disabled pp regions, etc.
  ///   #define FOO 1+2
  ///   #define BAR(a) a + 1
  ///   FOO    // invocation #1, tokens = {'1','+','2'}, macroTokens = {'FOO'}.
  ///   BAR(1) // invocation #2, tokens = {'a', '+', '1'},
  ///                            macroTokens = {'BAR', '(', '1', ')'}.
  struct Mapping {
    // Positions in the corresponding spelled token stream. The corresponding
    // range is never empty.
    unsigned BeginSpelled = 0;
    unsigned EndSpelled = 0;
    // Positions in the expanded token stream. The corresponding range can be
    // empty.
    unsigned BeginExpanded = 0;
    unsigned EndExpanded = 0;

    /// For debugging purposes.
    std::string str() const;
  };
  /// Spelled tokens of the file with information about the subranges.
  struct MarkedFile {
    /// Lexed, but not preprocessed, tokens of the file. These map directly to
    /// text in the corresponding files and include tokens of all preprocessor
    /// directives.
    /// FIXME: spelled tokens don't change across FileID that map to the same
    ///        FileEntry. We could consider deduplicating them to save memory.
    std::vector<syntax::Token> SpelledTokens;
    /// A sorted list to convert between the spelled and expanded token streams.
    std::vector<Mapping> Mappings;
    /// The first expanded token produced for this FileID.
    unsigned BeginExpanded = 0;
    unsigned EndExpanded = 0;
  };

  friend class TokenCollector;

  /// Maps a single expanded token to its spelled counterpart or a mapping that
  /// produced it.
  std::pair<const syntax::Token *, const Mapping *>
  spelledForExpandedToken(const syntax::Token *Expanded) const;

  /// Token stream produced after preprocessing, conceputally this captures the
  /// same stream as 'clang -E' (excluding the preprocessor directives like
  /// #file, etc.).
  std::vector<syntax::Token> ExpandedTokens;
  llvm::DenseMap<FileID, MarkedFile> Files;
  // The value is never null, pointer instead of reference to avoid disabling
  // implicit assignment operator.
  const SourceManager *SourceMgr;
};

/// Lex the text buffer, corresponding to \p FID, in raw mode and record the
/// resulting spelled tokens. Does minimal post-processing on raw identifiers,
/// setting the appropriate token kind (instead of the raw_identifier reported
/// by lexer in raw mode). This is a very low-level function, most users should
/// prefer to use TokenCollector. Lexing in raw mode produces wildly different
/// results from what one might expect when running a C++ frontend, e.g.
/// preprocessor does not run at all.
/// The result will *not* have a 'eof' token at the end.
std::vector<syntax::Token> tokenize(FileID FID, const SourceManager &SM,
                                    const LangOptions &LO);

/// Collects tokens for the main file while running the frontend action. An
/// instance of this object should be created on
/// FrontendAction::BeginSourceFile() and the results should be consumed after
/// FrontendAction::Execute() finishes.
class TokenCollector {
public:
  /// Adds the hooks to collect the tokens. Should be called before the
  /// preprocessing starts, i.e. as a part of BeginSourceFile() or
  /// CreateASTConsumer().
  TokenCollector(Preprocessor &P);

  /// Finalizes token collection. Should be called after preprocessing is
  /// finished, i.e. after running Execute().
  LLVM_NODISCARD TokenBuffer consume() &&;

private:
  /// Maps from a start to an end spelling location of transformations
  /// performed by the preprocessor. These include:
  ///   1. range from '#' to the last token in the line for PP directives,
  ///   2. macro name and arguments for macro expansions.
  /// Note that we record only top-level macro expansions, intermediate
  /// expansions (e.g. inside macro arguments) are ignored.
  ///
  /// Used to find correct boundaries of macro calls and directives when
  /// building mappings from spelled to expanded tokens.
  ///
  /// Logically, at each point of the preprocessor execution there is a stack of
  /// macro expansions being processed and we could use it to recover the
  /// location information we need. However, the public preprocessor API only
  /// exposes the points when macro expansions start (when we push a macro onto
  /// the stack) and not when they end (when we pop a macro from the stack).
  /// To workaround this limitation, we rely on source location information
  /// stored in this map.
  using PPExpansions = llvm::DenseMap</*SourceLocation*/ int, SourceLocation>;
  class Builder;
  class CollectPPExpansions;

  std::vector<syntax::Token> Expanded;
  // FIXME: we only store macro expansions, also add directives(#pragma, etc.)
  PPExpansions Expansions;
  Preprocessor &PP;
  CollectPPExpansions *Collector;
};

} // namespace syntax
} // namespace clang

#endif
