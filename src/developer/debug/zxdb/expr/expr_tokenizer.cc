// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/expr_tokenizer.h"

#include <ctype.h>
#include <lib/syslog/cpp/macros.h>

#include <type_traits>

#include "src/developer/debug/zxdb/expr/number_parser.h"
#include "src/developer/debug/zxdb/expr/parse_special_identifier.h"
#include "src/developer/debug/zxdb/expr/parse_string.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace zxdb {

namespace {

bool IsNameChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool IsNameFirstChar(char c) { return IsNameChar(c); }
bool IsNameContinuingChar(char c) { return IsNameChar(c) || (c >= '0' && c <= '9'); }

bool IsIntegerFirstChar(char c) { return isdigit(c); }

// This allows all alphanumeric characters for simplicity. Integer literals aren't validated at the
// tokenizer level and will be checked later. Our job is to find the extent of the literal.
bool IsIntegerContinuingChar(char c) { return isalnum(c); }

// Returns a list of all tokens sharing the given first character.
const std::vector<const ExprTokenRecord*>& TokensWithFirstChar(char c) {
  // Lookup table for all 7-bit characters.
  constexpr unsigned char kMaxLookupChar = 0x80;
  static std::vector<const ExprTokenRecord*> mapping[kMaxLookupChar];
  static bool initialized = false;

  if (!initialized) {
    // Construct the lookup table.
    initialized = true;
    for (size_t i = 0; i < kNumExprTokenTypes; i++) {
      ExprTokenType type = static_cast<ExprTokenType>(i);

      // Special-case the ">" since they can be ambiguous. Don't add ">>" as they will be
      // disambiguated by the parser.
      if (type == ExprTokenType::kShiftRight || type == ExprTokenType::kShiftRightEquals)
        continue;

      const ExprTokenRecord& record = RecordForTokenType(type);
      if (!record.static_value.empty())
        mapping[static_cast<size_t>(record.static_value[0])].push_back(&record);
    }
  }

  if (static_cast<unsigned char>(c) >= kMaxLookupChar) {
    static std::vector<const ExprTokenRecord*> empty_records;
    return empty_records;
  }
  return mapping[static_cast<size_t>(c)];
}

}  // namespace

ExprTokenizer::ExprTokenizer(const std::string& input, ExprLanguage lang)
    : input_(input), language_(lang) {}

bool ExprTokenizer::Tokenize() {
  while (!done()) {
    AdvanceToNextToken();
    if (done())
      break;

    if (auto string_info = DoesBeginStringLiteral(language_, input_, cur_)) {
      // String literals are handled specially by the string parser.
      auto result = ParseStringLiteral(input_, *string_info, &cur_, &error_location_);
      if (result.has_error()) {
        err_ = result.err();
        break;
      }

      tokens_.emplace_back(ExprTokenType::kStringLiteral, result.value(),
                           string_info->string_begin);
      continue;
    }

    // Special escaped identifiers.
    if (cur_char() == '$') {
      // Here just discard the name and contents. These will be re-extracted by the parser. This
      // could be optimized to avoid the extra work, but we don't need that level of optimization.
      size_t token_begin = cur_;
      SpecialIdentifier special = SpecialIdentifier::kNone;
      std::string special_cont;
      err_ = ParseSpecialIdentifier(input_, &cur_, &special, &special_cont, &error_location_);
      if (err_.has_error())
        break;
      tokens_.emplace_back(ExprTokenType::kSpecialName,
                           input_.substr(token_begin, cur_ - token_begin), token_begin);
      continue;
    }

    // Floats.
    if (size_t float_len = GetFloatTokenLength(language_, input_.substr(cur_))) {
      tokens_.emplace_back(ExprTokenType::kFloat, input_.substr(cur_, float_len), cur_);
      cur_ += float_len;
      continue;
    }

    const ExprTokenRecord& record = ClassifyCurrent();
    if (has_error())
      break;

    size_t token_begin = cur_;
    AdvanceToEndOfToken(record);
    if (has_error())
      break;

    size_t token_end = cur_;
    std::string token_value(&input_[token_begin], token_end - token_begin);
    tokens_.emplace_back(record.type, token_value, token_begin);
  }
  return !has_error();
}

// static
bool ExprTokenizer::IsNameToken(ExprLanguage lang, std::string_view input) {
  if (input.empty())
    return false;

  // Check the first character.
  size_t i;  // Index to start subsequent checking at.
  if (lang == ExprLanguage::kC && input.size() >= 2 && input[0] == '~') {
    // See the comment about tilde handling in C in ClassifyCurrent().
    if (!IsNameFirstChar(input[1]))
      return false;
    i = 2;
  } else {
    if (!IsNameFirstChar(input[0]))
      return false;
    i = 1;
  }

  for (; i < input.size(); i++) {
    if (!IsNameContinuingChar(input[i]))
      return false;
  }
  return true;
}

// static
std::string ExprTokenizer::GetErrorContext(const std::string& input, size_t byte_offset) {
  // Index should be in range of the input string. Also allow indicating one
  // character past the end.
  FX_DCHECK(byte_offset <= input.size());

  // Future enhancements:
  // - If we allow multiline expressions in the input, the returned context should not cross
  //   newlines or it will be messed up.
  // - Input longer than 80 chars should be clipped to guarantee it doesn't wrap.

  std::string output;
  output = "  " + input + "\n  ";
  output.append(byte_offset, ' ');
  output.push_back('^');
  return output;
}

void ExprTokenizer::AdvanceChars(int n) { cur_ += n; }

void ExprTokenizer::AdvanceOneChar() { cur_++; }

void ExprTokenizer::AdvanceToNextToken() {
  while (!at_end() && IsCurrentWhitespace())
    AdvanceOneChar();
}

void ExprTokenizer::AdvanceToEndOfToken(const ExprTokenRecord& record) {
  if (!record.static_value.empty()) {
    // Known sizes. Because the token matched we should always have enough characters.
    FX_DCHECK(input_.size() >= cur_ + record.static_value.size());
    cur_ += record.static_value.size();
    return;
  }

  // Manually advance over variable-length tokens.
  switch (record.type) {
    case ExprTokenType::kInteger:
      do {
        AdvanceOneChar();
      } while (!at_end() && IsIntegerContinuingChar(cur_char()));
      break;

    case ExprTokenType::kName:
      do {
        AdvanceOneChar();
      } while (!at_end() && IsNameContinuingChar(cur_char()));
      break;

    default:
      FX_NOTREACHED();
      err_ = Err("Internal parser error.");
      error_location_ = cur_;
      break;
  }
}

bool ExprTokenizer::CurrentMatchesTokenRecord(const ExprTokenRecord& record) const {
  // Non-statically-known tokens shouldn't use this code path.
  FX_DCHECK(!record.static_value.empty());

  const size_t size = record.static_value.size();
  if (!can_advance(size))
    return false;  // Not enough room.

  if (!(record.languages & static_cast<unsigned>(language_)))
    return false;  // Doesn't apply to this language.

  if (std::string_view(&input_[cur_], size) != record.static_value)
    return false;  // Doesn't match the token static value.

  if (record.is_alphanum) {
    if (cur_ + size < input_.size() && isalnum(input_[cur_ + size]))
      return false;  // Alphanumeric character follows so won't match.
  }

  return true;
}

bool ExprTokenizer::IsCurrentWhitespace() const {
  FX_DCHECK(!at_end());
  char c = input_[cur_];
  return c == 0x0A || c == 0x0D || c == 0x20;
}

const ExprTokenRecord& ExprTokenizer::ClassifyCurrent() {
  FX_DCHECK(!at_end());
  char cur = cur_char();

  // Tilde is tricky in C++ because in different contexts ~Foo could be a destructor name or the
  // bitwise not of a variable called "Foo". The compiler disambiguates by doing a lookup on the
  // whether Foo is a type name or not.
  //
  // In the debugger we can't be so sure. We parse function names (including destructors) in
  // contexts where we don't have any symbol information, so have to be able to handle destructor
  // names at any time.
  //
  // As a result, we treat all ~ followed by a word as a word. If there's a space or another
  // operator in between, we'll treat it as a unary bitwise not.
  if (language_ == ExprLanguage::kC && cur == '~') {
    if (can_advance(1) && IsNameFirstChar(input_[cur_ + 1]))
      return RecordForTokenType(ExprTokenType::kName);
  }

  // Compare against known tokens.
  const ExprTokenRecord* longest = nullptr;
  for (const ExprTokenRecord* match : TokensWithFirstChar(cur)) {
    if (!CurrentMatchesTokenRecord(*match))
      continue;

    if (!longest || match->static_value.size() > longest->static_value.size())
      longest = match;
  }

  if (longest)
    return *longest;

  // Integers.
  if (IsIntegerFirstChar(cur))
    return RecordForTokenType(ExprTokenType::kInteger);

  // Everything else is a general name.
  if (IsNameFirstChar(cur))
    return RecordForTokenType(ExprTokenType::kName);

  error_location_ = cur_;
  err_ = Err(fxl::StringPrintf("Invalid character '%c' in expression.\n", cur) +
             GetErrorContext(input_, cur_));
  return RecordForTokenType(ExprTokenType::kInvalid);
}

}  // namespace zxdb
