// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "html-rewriter-selector.h"

#include <kj/vector.h>

namespace workerd::api::htmlrewriter {
namespace {

bool isCssWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool isHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

struct SelectorContext {
  size_t combinators = 0;
  bool canEndCompound = false;
  bool pendingWhitespace = false;
};

size_t consumeCssEscape(kj::StringPtr selector, size_t i) {
  if (++i >= selector.size()) {
    return i;
  }

  if (!isHexDigit(selector[i])) {
    if (selector[i] == '\r' && i + 1 < selector.size() && selector[i + 1] == '\n') {
      ++i;
    }
    return i;
  }

  size_t digits = 1;
  while (i + 1 < selector.size() && digits < 6 && isHexDigit(selector[i + 1])) {
    ++i;
    ++digits;
  }
  if (i + 1 < selector.size() && isCssWhitespace(selector[i + 1])) {
    ++i;
    if (selector[i] == '\r' && i + 1 < selector.size() && selector[i + 1] == '\n') {
      ++i;
    }
  }
  return i;
}

size_t consumeCssComment(kj::StringPtr selector, size_t i) {
  i += 2;
  while (i + 1 < selector.size() && (selector[i] != '*' || selector[i + 1] != '/')) {
    ++i;
  }
  return i + 1 < selector.size() ? i + 1 : i;
}

bool addCombinator(SelectorContext& context) {
  return ++context.combinators > MAX_SELECTOR_COMPLEXITY;
}

}  // namespace

bool selectorExceedsComplexityLimit(kj::StringPtr selector) {
  kj::Vector<SelectorContext> contexts;
  contexts.add(SelectorContext{});

  size_t bracketDepth = 0;
  char quote = '\0';

  for (size_t i = 0; i < selector.size(); ++i) {
    char c = selector[i];

    if (quote != '\0') {
      if (c == '\\') {
        i = consumeCssEscape(selector, i);
      } else if (c == quote) {
        quote = '\0';
      }
      continue;
    }

    if (c == '/' && i + 1 < selector.size() && selector[i + 1] == '*') {
      i = consumeCssComment(selector, i);
      continue;
    }

    if (c == '\\') {
      auto& context = contexts.back();
      if (context.pendingWhitespace && addCombinator(context)) {
        return true;
      }
      context.pendingWhitespace = false;
      context.canEndCompound = true;
      i = consumeCssEscape(selector, i);
      continue;
    }

    if (bracketDepth > 0) {
      if (c == '\'' || c == '"') {
        quote = c;
      } else if (c == '[') {
        ++bracketDepth;
      } else if (c == ']') {
        --bracketDepth;
      }
      continue;
    }

    auto& context = contexts.back();
    if (isCssWhitespace(c)) {
      context.pendingWhitespace |= context.canEndCompound;
      continue;
    }

    if (c == '>' || c == '+' || c == '~') {
      context.pendingWhitespace = false;
      context.canEndCompound = false;
      if (addCombinator(context)) {
        return true;
      }
      continue;
    }

    if (c == ',') {
      context = SelectorContext{};
      continue;
    }

    if (c == ')') {
      if (contexts.size() > 1) {
        contexts.removeLast();
        contexts.back().canEndCompound = true;
        contexts.back().pendingWhitespace = false;
      }
      continue;
    }

    if (context.pendingWhitespace && addCombinator(context)) {
      return true;
    }
    context.pendingWhitespace = false;
    context.canEndCompound = true;

    if (c == '\'' || c == '"') {
      quote = c;
    } else if (c == '[') {
      bracketDepth = 1;
    } else if (c == '(') {
      if (contexts.size() > MAX_SELECTOR_COMPLEXITY) {
        return true;
      }
      contexts.add(SelectorContext{});
    }
  }

  return false;
}

}  // namespace workerd::api::htmlrewriter
