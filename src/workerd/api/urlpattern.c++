// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "urlpattern.h"
#include "url-standard.h"
#include "util.h"
#include <kj/vector.h>
#include <unicode/uchar.h>
#include <algorithm>

namespace workerd::api {

namespace {

// The parsing and matching algorithm used for URLPattern's is fairly complex. Here's a summary.
//
// Internally, a URLPattern consists of 8 individual components, each of which
// are derived from it's own Input Pattern and match against specific pieces of
// a URL. The components are:
//
// * Protocol
// * Username
// * Password
// * Hostname
// * Port
// * Pathname
// * Search
// * Hash
//
// When a URLPattern object is created, users can choose to pass in either an object
// with each individual component pattern separately described (or omitted)
//
//   const pattern = new URLPattern({
//     protocol: "*",
//     pathname: "/foo/(\d+)"
//   });
//
// Or as a string:
//
//  const pattern = new URLPattern("*://*/foo/(\d+)");
//
// If a string is passed, the constructor will first parse that string to determine
// the boundaries of each of the component parts. Internally, this effectively builds
// the equivalent object version of the constructor input, normalizing the inputs as
// it goes.
//
// Once all of the individual input patterns are identified, each individual pattern
// is parsed to generate the internal component that will be used for matching.
// (Yes, this means that when a string is passed in to the URLPattern constructor,
// it ends up being parsed over multiple times).
//
// Each of the individual components consists of a pattern string, a computed
// JavaScript regular expression, and a list of names derived from the pattern.
// When the URLPattern is executed against an input URL, each of the 8 different
// component Regular Expressions are evaluated against each component of the
// URL. If any of those component regular expressions does not match its
// corresponding input, then the URLPattern test/exec will fail. If all components
// generate a matching result, however, the URLPattern will compile the results
// and return those to the caller.
//
// The implementation here is nearly a line-for-line implementation of exactly
// what the URLPattern specification says, which might not be the most efficient
// possible implementation. The spec itself even accounts for this by allowing
// implementations to use more performant implementations so long as the observable
// behavior remains compliant. There is likely plenty of room to optimize here!
//
// The implementation builds on the new spec-compliant URL parser but does not
// require the compatibility flag to be enabled. It will use the new parser
// internally.

using RegexAndNameList = std::pair<jsg::V8Ref<v8::RegExp>, kj::Array<jsg::UsvString>>;

constexpr const char* SYNTAX_ERROR = "Syntax error in URLPattern";
constexpr const char* BASEURL_ERROR = "A baseURL is not allowed when input is an object.";

struct Common {
  jsg::UsvString DUMMY_PROTOCOL = jsg::usv('d', 'u', 'm', 'm', 'y');
  jsg::UsvString FULL_WILDCARD = jsg::usv('.', '*');
  jsg::UsvString WILDCARD = jsg::usv('*');
  jsg::UsvString EMPTY_PATH = jsg::usv('/');
  jsg::UsvString DUMMY_URL = jsg::usv(':', '/', '/', 'd', 'u', 'm', 'm', 'y',
                                        '.', 't', 'e', 's', 't');
};

Common& getCommonStrings() {
  static Common common;
  return common;
}

using EncodingCallback =
    kj::Function<jsg::UsvString(jsg::UsvStringPtr, kj::Maybe<jsg::UsvStringPtr>)>;

struct CompileOptions {
  // Options used internally when compiling a URLPattern component
  kj::Maybe<uint32_t> delimiterCodePoint;
  kj::Maybe<uint32_t> prefixCodePoint;

  CompileOptions(kj::Maybe<uint32_t> delimiterCodePoint = nullptr,
                 kj::Maybe<uint32_t> prefixCodePoint = nullptr)
      : delimiterCodePoint(kj::mv(delimiterCodePoint)),
        prefixCodePoint(kj::mv(prefixCodePoint)) {}

  static const CompileOptions DEFAULT;
  static const CompileOptions HOSTNAME;
  static const CompileOptions PATHNAME;
};

const CompileOptions CompileOptions::DEFAULT(nullptr, nullptr);
const CompileOptions CompileOptions::HOSTNAME('.');
const CompileOptions CompileOptions::PATHNAME('/', '/');

struct Token {
  // String inputs passed into URLPattern constructor are parsed by first
  // interpreting them into a list of Tokens. Each token has a type, a
  // position index in the input string, and a value. The value is either
  // a individual codepoint or a substring of input. Once the tokens are
  // determined, the parsing algorithms convert those into a Part list.
  // The part list is then used to generate the internal JavaScript RegExps
  // that are used for the actual matching operation.

  enum class Policy {
    // Per the URLPattern spec, the tokenizer runs in one of two modes:
    // Strict and Lenient. In Strict mode, invalid characters and sequences
    // detected by the tokenizer will cause a TypeError to be thrown.
    // In lenient mode, the invalid codepoints and sequences are marked
    // but no error is thrown. When parsing a string passed to the
    // URLPattern constructor, lenient mode is used. When parsing the
    // pattern string for an individual component, strict mode is used.
    STRICT,
    LENIENT,
  };

  enum class Type {
    INVALID_CHAR,
    OPEN,
    CLOSE,
    REGEXP,
    NAME,
    CHAR,
    ESCAPED_CHAR,
    OTHER_MODIFIER,
    ASTERISK,
    END,
  };

  Type type = Type::INVALID_CHAR;
  size_t index = 0;
  kj::OneOf<uint32_t, jsg::UsvStringPtr> value = uint32_t(0);

  // Utility function useful when debugging. Uncomment if you need to
  // add any KJ_DBG statements to check which Tokens have been generated
  // by the tokenizer algorithm... Commented out here because it's not
  // used in the regular implementation but useful to keep around for
  // debugging.
  //
  // kj::String typeName() {
  //   switch (type) {
  //     case Type::INVALID_CHAR: return kj::str("INVALID_CHAR");
  //     case Type::OPEN: return kj::str("OPEN");
  //     case Type::CLOSE: return kj::str("CLOSE");
  //     case Type::REGEXP: return kj::str("REGEXP");
  //     case Type::NAME: return kj::str("NAME");
  //     case Type::CHAR: return kj::str("CHAR");
  //     case Type::ESCAPED_CHAR: return kj::str("ESCAPED_CHAR");
  //     case Type::OTHER_MODIFIER: return kj::str("OTHER_MODIFIER");
  //     case Type::ASTERISK: return kj::str("ASTERISK");
  //     case Type::END: return kj::str("END");
  //   }
  //   return kj::str("<unknown>");
  // }

  jsg::UsvString tokenValue() {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(codepoint, uint32_t) {
        return jsg::usv(codepoint);
      }
      KJ_CASE_ONEOF(ptr, jsg::UsvStringPtr) {
        return jsg::usv(ptr);
      }
    }
    KJ_UNREACHABLE;
  }

  bool operator==(jsg::UsvString& other) {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(codepoint, uint32_t) {
        return false;
      }
      KJ_CASE_ONEOF(string, jsg::UsvStringPtr) {
        return other == string;
      }
    }
    KJ_UNREACHABLE;
  }

  bool operator==(uint32_t other) {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(codepoint, uint32_t) {
        return codepoint == other;
      }
      KJ_CASE_ONEOF(string, jsg::UsvStringPtr) {
        return false;
      }
    }
    KJ_UNREACHABLE;
  }

  bool operator!=(jsg::UsvString& other) {
    return !(*this == other);
  }

  bool operator!=(uint32_t other) {
    return !(*this == other);
  }

  static Token asterisk(size_t index) {
    return {
      .type = Type::ASTERISK,
      .index = index,
      .value = uint32_t('*'),
    };
  }

  static Token char_(size_t index, uint32_t codepoint) {
    return {
      .type = Type::CHAR,
      .index = index,
      .value = codepoint,
    };
  }

  static Token close(size_t index) {
    return {
      .type = Type::CLOSE,
      .index = index,
    };
  }

  static Token end(size_t index) {
    return {
      .type = Type::END,
      .index = index,
    };
  }

  static Token escapedChar(size_t index, uint32_t codepoint) {
    return {
      .type = Type::ESCAPED_CHAR,
      .index = index,
      .value = codepoint,
    };
  }

  static Token invalidChar(size_t index, uint32_t codepoint) {
    return {
      .index = index,
      .value = codepoint,
    };
  }

  static Token invalidSegment(size_t index, jsg::UsvStringPtr segment) {
    return {
      .type = Type::INVALID_CHAR,
      .index = index,
      .value = segment,
    };
  }

  static Token name(size_t index, jsg::UsvStringPtr name) {
    return {
      .type = Type::NAME,
      .index = index,
      .value = name,
    };
  }

  static Token open(size_t index) {
    return {
      .type = Type::OPEN,
      .index = index,
    };
  }

  static Token otherModifier(size_t index, uint32_t codepoint) {
    return {
      .type = Type::OTHER_MODIFIER,
      .index = index,
      .value = codepoint,
    };
  }

  static Token regex(size_t index, jsg::UsvStringPtr regex) {
    return {
      .type = Type::REGEXP,
      .index = index,
      .value = regex,
    };
  }
};

struct Part {
  // An individual piece of a URLPattern string. Used while parsing a URLPattern
  // string for the URLPattern constructor, test, or exec call.
  enum class Type {
    FIXED_TEXT,
    REGEXP,
    SEGMENT_WILDCARD,
    FULL_WILDCARD,
  };

  enum class Modifier {
    NONE,
    OPTIONAL,      // ?
    ZERO_OR_MORE,  // *
    ONE_OR_MORE,   // +
  };

  Type type;
  Modifier modifier;
  jsg::UsvString value;
  jsg::UsvString name;
  jsg::UsvString prefix;
  jsg::UsvString suffix;

  // Utility function useful when debugging. Uncomment if you need to
  // add any KJ_DBG statements to check which Parts have been generated
  // by the parsing algorithms... Commented out here because it's not
  // used in the regular implementation but useful to keep around for
  // debugging.
  //
  // kj::String typeName() {
  //   switch (type) {
  //     case Type::FIXED_TEXT: return kj::str("FIXED_TEXT");
  //     case Type::REGEXP: return kj::str("REGEXP");
  //     case Type::SEGMENT_WILDCARD: return kj::str("SEGMENT_WILDCARD");
  //     case Type::FULL_WILDCARD: return kj::str("FULL_WILDCARD");
  //   }
  //   return kj::str("<unknown>");
  // }
};

enum class ProcessPatternInitType {
  PATTERN,
  URL,
};

kj::Maybe<uint32_t> modifierToCodepoint(Part::Modifier modifier) {
  switch (modifier) {
    case Part::Modifier::ZERO_OR_MORE: return '*';
    case Part::Modifier::OPTIONAL: return '?';
    case Part::Modifier::ONE_OR_MORE: return '+';
    case Part::Modifier::NONE: return nullptr;
  }
  KJ_UNREACHABLE;
};

Part::Modifier maybeTokenToModifier(kj::Maybe<Token&> modifierToken) {
  KJ_IF_MAYBE(token, modifierToken) {
    KJ_ASSERT(token->type == Token::Type::OTHER_MODIFIER ||
              token->type == Token::Type::ASTERISK);
    if (*token == '?') {
      return Part::Modifier::OPTIONAL;
    } else if (*token == '*') {
      return Part::Modifier::ZERO_OR_MORE;
    } else if (*token == '+') {
      return Part::Modifier::ONE_OR_MORE;
    }
    KJ_UNREACHABLE;
  }
  return Part::Modifier::NONE;
}

#define SPECIAL_SCHEME(V) \
  V(https) \
  V(http)  \
  V(ws)    \
  V(wss)   \
  V(ftp)   \
  V(file)  \

// This function is a bit unfortunate. It is required by the specification. What is it doing
// is checking to see if the compiled regular expression for a protocol component matches
// any of the special protocol schemes. To do so, it has to execute the regular expression
// multiple times, once per scheme, until it finds a match. The SPECIAL_SCHEME macro has been
// ordered to make it so the *most likely* matches will be checked first.
// TODO (later): Investigate whether there is a more efficient way to handle this.
bool protocolComponentMatchesSpecialScheme(jsg::Lock& js, URLPatternComponent& component) {
  auto handle = component.regex.getHandle(js);
  auto context = js.v8Context();

  const auto checkIt = [&handle, &js, &context](const char* name) {
    return !jsg::check(handle->Exec(context, jsg::v8StrIntern(js.v8Isolate, name)))
        ->IsNullOrUndefined();
  };

  return js.tryCatch([&] {
#define V(name) if (checkIt(#name)) return true;
  SPECIAL_SCHEME(V)
#undef V
    return false;
  }, [&](auto&& exception) {
    // We ignore the exception here and just return false;
    return false;
  });
}
#undef SPECIAL_SCHEME

bool isSpecialSchemeDefaultPort(jsg::UsvStringPtr protocol, jsg::UsvStringPtr port) {
  KJ_IF_MAYBE(defaultPort, url::URL::defaultPortForScheme(protocol)) {
    return port == jsg::usv(kj::toCharSequence(*defaultPort));
  }
  return false;
}

bool isIpv6(jsg::UsvStringPtr hostname) {
  // This is not meant to be a comprehensive validation that the hostname is
  // a proper IPv6 address. It's a quick check defined by the URLPattern spec.
  if (hostname.size() < 2) return false;
  auto c1 = hostname.getCodepointAt(0);
  auto c2 = hostname.getCodepointAt(1);
  return (c1 == '[' || ((c1 == '{' || c1 == '\\') && c2 == '['));
}

bool isAsciiDigit(auto c) { return c >= '0' && c <= '9'; };

jsg::UsvString canonicalizeProtocol(jsg::UsvStringPtr input, kj::Maybe<jsg::UsvStringPtr>) {
  if (input.size() == 0) return jsg::usv();
  url::UrlRecord dummyUrl;

  auto& suffix = getCommonStrings().DUMMY_URL;

  jsg::UsvStringBuilder builder(input.size() + suffix.size());
  builder.addAll(input);
  builder.addAll(suffix);

  auto str = builder.finish();

  auto result = JSG_REQUIRE_NONNULL(
      url::URL::parse(str, nullptr, dummyUrl),
      TypeError,
      "Invalid protocol scheme.");

  return kj::mv(result.scheme);
}

jsg::UsvString canonicalizeUsername(jsg::UsvStringPtr input, kj::Maybe<jsg::UsvStringPtr>) {
  if (input.size() == 0) return jsg::usv();
  url::UrlRecord dummyUrl;
  dummyUrl.setUsername(input);
  return kj::mv(dummyUrl.username);
}

jsg::UsvString canonicalizePassword(jsg::UsvStringPtr input, kj::Maybe<jsg::UsvStringPtr>) {
  if (input.size() == 0) return jsg::usv();
  url::UrlRecord dummyUrl;
  dummyUrl.setPassword(input);
  return kj::mv(dummyUrl.password);
}

jsg::UsvString canonicalizeHostname(jsg::UsvStringPtr input, kj::Maybe<jsg::UsvStringPtr>) {
  if (input.size() == 0) return jsg::usv();
  url::UrlRecord dummyUrl;

  // This additional check deals with a known bug in the URLPattern spec. The URL parser will
  // allow (and generally ignore) invalid characters in the hostname when running with the
  // HOST state override. The URLPattern spec, however, assumes that it doesn't.
  if (!isIpv6(input)) {
    const auto isForbiddenHostCodepoint = [](auto c) {
      return c == 0x00 || c == 0x09 /* Tab */ || c == 0x0a /* LF */ || c == 0x0d /* CR */ ||
            c == ' ' || c == '#' || c == '%' || c == '/' || c == ':' ||
            c == '<' || c == '>' || c == '?' || c == '@' || c == '[' || c == '\\' || c == ']' ||
            c == '^' || c == '|';
    };
    auto it = input.begin();
    while (it) {
      JSG_REQUIRE(!isForbiddenHostCodepoint(*it), TypeError, "Invalid URL hostname component.");
      ++it;
    }
  }

  auto result = JSG_REQUIRE_NONNULL(
      url::URL::parse(input, nullptr, dummyUrl, url::URL::ParseState::HOSTNAME),
      TypeError,
      "Invalid URL hostname component.");

  return kj::mv(result.host).orDefault(jsg::usv());
}

jsg::UsvString canonicalizeIpv6Hostname(jsg::UsvStringPtr input, kj::Maybe<jsg::UsvStringPtr>) {
  jsg::UsvStringBuilder result(input.size());
  auto it = input.begin();
  while (it) {
    auto c = *it;
    JSG_REQUIRE(isHexDigit(c) || c == '[' || c == ']' || c == ':',
                 TypeError, kj::str(SYNTAX_ERROR, ": Invalid IPv6 address."));
    result.add(u_tolower(c));
    ++it;
  }
  return result.finish();
}

jsg::UsvString canonicalizePort(
    jsg::UsvStringPtr input,
    kj::Maybe<jsg::UsvStringPtr> maybeProtocol) {
  if (input.size() == 0) return jsg::usv();
  url::UrlRecord dummyUrl;
  KJ_IF_MAYBE(protocol, maybeProtocol) {
    dummyUrl.scheme = jsg::usv(*protocol);
  }

  // This following scan is not explicitly in the spec, which doesn't seem
  // to account for the fact that trailing invalid characters are ignore
  // by the URL basic parser when the PORT state override is given. The
  // URL Pattern web platform tests appear to forget this!
  auto it = input.begin();
  while (it) {
    JSG_REQUIRE(isAsciiDigit(*it), TypeError, "Invalid URL port component.");
    ++it;
  }
  // There are no valid ports long than 5 digits in length.
  JSG_REQUIRE(input.size() < 6, TypeError, "Invalid URL port component.");

  auto result = JSG_REQUIRE_NONNULL(
    url::URL::parse(input, nullptr, dummyUrl, url::URL::ParseState::PORT),
    TypeError,
    "Invalid URL port component.");

  // This is not super clear from the specs so an explanation is helpful.
  // If any port value is specified but is invalid, the fact that we're
  // passing a state override in the parse above means that, in some cases,
  // parse will just return with an empty port. However, parse will also
  // return an empty port if the input is the default port for the given
  // scheme. This doesn't seem to be accounted for in the specs! So we're
  // adding an additional couple of checks here to ensure that the web
  // platform tests pass!
  KJ_IF_MAYBE(protocol, maybeProtocol) {
    if (!isSpecialSchemeDefaultPort(*protocol, input)) {
      // In this case, we require that the port return not null! If the input
      // did equal the default port, then we'd fully expect it to be null!
      auto port = JSG_REQUIRE_NONNULL(result.port, TypeError, "Invalid URL port.");
      return jsg::usv(kj::toCharSequence(port));
    }
  } else {
    // In this case, we can't check that the value is the default port, but
    // also neither can the URL parser. The only time a null port should be
    // returned here is if the input valid was invalid.
    auto port = JSG_REQUIRE_NONNULL(result.port, TypeError, "Invalid URL port.");
    return jsg::usv(kj::toCharSequence(port));
  }

  // Here, it's ok for the result.port to be null because it's likely the
  // default port for the protocol scheme specified.
  KJ_IF_MAYBE(port, result.port) {
    return jsg::usv(kj::toCharSequence(*port));
  }
  return jsg::usv();
}

jsg::UsvString canonicalizePathname(jsg::UsvStringPtr input, kj::Maybe<jsg::UsvStringPtr>) {
  if (input.size() == 0) return jsg::usv();
  url::UrlRecord dummyUrl;
  auto result = JSG_REQUIRE_NONNULL(
    url::URL::parse(input, nullptr, dummyUrl, url::URL::ParseState::PATH_START),
    TypeError,
    "Invalid URL pathname component.");

  auto path = result.getPathname();

  if (input.first() != '/') {
    KJ_ASSERT(path.first() == '/');
    return jsg::usv(path.slice(1));
  }
  return kj::mv(path);
}

jsg::UsvString canonicalizeOpaquePathname(
    jsg::UsvStringPtr input,
    kj::Maybe<jsg::UsvStringPtr>) {
  if (input.size() == 0) return jsg::usv();
  url::UrlRecord dummyUrl;
  dummyUrl.path = jsg::usv();
  auto result = JSG_REQUIRE_NONNULL(
      url::URL::parse(input, nullptr, dummyUrl, url::URL::ParseState::OPAQUE_PATH),
      TypeError,
      "Invalid URL opaque path component.");
  return result.getPathname();
}

jsg::UsvString canonicalizeSearch(jsg::UsvStringPtr input, kj::Maybe<jsg::UsvStringPtr>) {
  if (input.size() == 0) return jsg::usv();
  url::UrlRecord dummyUrl;
  dummyUrl.query = jsg::usv();
  auto result = JSG_REQUIRE_NONNULL(
    url::URL::parse(input, nullptr, dummyUrl, url::URL::ParseState::QUERY),
    TypeError,
    "Invalid URL search component.");
  KJ_IF_MAYBE(query, result.query) {
    return kj::mv(*query);
  }
  return jsg::usv();
}

jsg::UsvString canonicalizeHash(jsg::UsvStringPtr input, kj::Maybe<jsg::UsvStringPtr>) {
  if (input.size() == 0) return jsg::usv();
  url::UrlRecord dummyUrl;
  dummyUrl.fragment = jsg::usv();
  auto result = JSG_REQUIRE_NONNULL(
    url::URL::parse(input, nullptr, dummyUrl, url::URL::ParseState::FRAGMENT),
    TypeError,
    "Invalid URL hash component.");
  KJ_IF_MAYBE(fragment, result.fragment) {
    return kj::mv(*fragment);
  }
  return jsg::usv();
}

jsg::UsvString escape(jsg::UsvStringPtr str, auto predicate) {
  // Best case we don't have to escape anything so size remains the same,
  // but let's pad a little just in case.
  jsg::UsvStringBuilder result(str.size() + 10);
  auto it = str.begin();
  while (it) {
    auto c = *it;
    if (predicate(c)) result.add('\\');
    result.add(c);
    ++it;
  }
  return result.finish();
}

jsg::UsvString escapeRegexString(jsg::UsvStringPtr str) {
  return escape(str, [](auto c) {
    return c == '.' || c == '+' || c == '*' || c == '?' || c == '^' ||
           c == '$' || c == '{' || c == '}' || c == '(' || c == ')' ||
           c == '[' || c == ']' || c == '|' || c == '/' || c == '\\';
  });
}

jsg::UsvString escapePatternString(jsg::UsvStringPtr str) {
  return escape(str, [](auto c) {
    return c == '+' || c == '*' || c == '?' || c == ':' ||
        c == '{' || c == '}' || c == '(' || c == ')' ||
        c == '\\';
  });
}

jsg::UsvString generateSegmentWildcardRegexp(const CompileOptions& options) {
  jsg::UsvStringBuilder result(6);
  result.add('[', '^');
  KJ_IF_MAYBE(codepoint, options.delimiterCodePoint) {
    result.add('\\', *codepoint);
  }
  result.add(']', '+');
  return result.finish();
};

bool isValidCodepoint(auto codepoint, bool first) {
  // https://tc39.es/ecma262/#prod-IdentifierStart
  if (first) {
    return codepoint == '$' ||
           codepoint == '_' ||
           u_hasBinaryProperty(codepoint, UCHAR_ID_START);
  }
  return codepoint == '$' ||
         codepoint == 0x200C || // Zero-width non-joiner
         codepoint == 0x200D || // Zero-width joiner
         u_hasBinaryProperty(codepoint, UCHAR_ID_CONTINUE);
};

kj::Array<Token> tokenize(jsg::UsvStringPtr input, Token::Policy policy) {
  auto it = input.begin();
  kj::Vector<Token> tokenList(input.size() + 1);
  size_t pos = 0;

  const auto processCodepointError = [&tokenList, &policy, &input](auto pos, auto codepoint) {
    if (policy == Token::Policy::STRICT) {
      JSG_FAIL_REQUIRE(TypeError,
          kj::str(SYNTAX_ERROR, ": Unexpected codepoint (",
                  codepoint ,") in input [", input, "]."));
    }
    tokenList.add(Token::invalidChar(pos, codepoint));
  };

  const auto processSegmentError = [&tokenList, &policy, &input](auto start, auto end) {
    if (policy == Token::Policy::STRICT) {
      JSG_FAIL_REQUIRE(TypeError, kj::str(SYNTAX_ERROR, ": Invalid segment in input [",
                                           input, "]."));
    }
    tokenList.add(Token::invalidSegment(start, input.slice(start, end)));
  };

  const auto isAscii = [](auto codepoint) -> bool const {
    return codepoint >= 0x00 && codepoint <= 0x7f;
  };

  while (it) {
    auto c = *it;
    switch (c) {
      case '*': {
        tokenList.add(Token::asterisk(pos++));
        ++it;
        continue;
      }
      case '?':
        KJ_FALLTHROUGH;
      case '+': {
        tokenList.add(Token::otherModifier(pos++, c));
        ++it;
        continue;
      }
      case '\\': {
        if (!(++it)) {
          // Hit the end! Invalid escaped character
          processCodepointError(pos++, c);
          continue;
        }
        tokenList.add(Token::escapedChar(pos, *it));
        pos += 2;
        ++it;
        continue;
      }
      case '{': {
        tokenList.add(Token::open(pos++));
        ++it;
        continue;
      }
      case '}': {
        tokenList.add(Token::close(pos++));
        ++it;
        continue;
      }
      case ':': {
        if (!(++it)) {
          processCodepointError(pos++, c);
          continue;
        }
        auto nameStart = ++pos;
        auto namePosition = nameStart;
        while (it) {
          auto nc = *it;
          if (!isValidCodepoint(nc, nameStart == namePosition)) {
            break;
          }
          ++namePosition;
          ++pos;
          ++it;
        }
        if (namePosition == nameStart) {
          // There was a name token suffix without a valid name! Oh, the inhumanity of it all.
          processCodepointError(pos - 1, c);
        } else {
          tokenList.add(Token::name(nameStart - 1, input.slice(nameStart, namePosition)));
        }
        // We purposefully don't increment the iterator here
        // because we're already at the next position.
        continue;
      }
      case '(': {
        if (!(++it)) {
          processCodepointError(pos++, c);
          continue;
        }
        auto depth = 1;
        auto regexStart = ++pos;
        auto regexPosition = regexStart;
        auto error = false;
        while (it) {
          auto rc = *it;
          if (!isAscii(rc)) {
            processCodepointError(pos, rc);
            error = true;
            break;
          } else if (regexPosition == regexStart && rc == '?') {
            processCodepointError(pos, rc);
            error = true;
            break;
          } else if (rc == '\\') {
            if (!(++it)) {
              // Invalid escape character at end of input
              processCodepointError(pos++, rc);
              error = true;
              break;
            }
            ++pos;
            rc = *it;
            if (!isAscii(rc)) {
              processCodepointError(pos, rc);
              error = true;
              break;
            }
            regexPosition += 2;
            ++pos;
            ++it;
            continue;
          } else if (rc == ')') {
            if (--depth == 0) {
              ++pos;
              ++it;
              break;
            }
          } else if (rc == '(') {
            depth++;
            if (!(++it)) {
              processCodepointError(pos++, *(it - 1));
              error = true;
              break;
            }
            ++pos;
            ++regexPosition;
            rc = *it;
            if (rc != '?') {
              processCodepointError(pos, rc);
              error = true;
              break;
            }
          }
          ++it;
          ++pos;
          ++regexPosition;
        }
        if (error) continue;
        if (depth > 0 || regexStart == regexPosition) {
          processSegmentError(regexStart, pos);
          continue;
        }
        tokenList.add(Token::regex(regexStart - 1, input.slice(regexStart, regexPosition)));
        continue;
      }
    }
    tokenList.add(Token::char_(pos++, c));
    ++it;
  }
  tokenList.add(Token::end(input.size()));
  return tokenList.releaseAsArray();
}

kj::Array<Part> parsePatternString(
    jsg::UsvStringPtr input,
    EncodingCallback encodingCallback,
    const CompileOptions& options) {
  kj::Array<Token> tokenList = tokenize(input, Token::Policy::STRICT);
  jsg::UsvString segmentWildcardRegex = generateSegmentWildcardRegexp(options);
  jsg::UsvStringBuilder pendingFixedValue(64);
  kj::Vector<Part> partList(tokenList.size());
  size_t index = 0;
  int nextNumericName = 0;

  const auto tryConsumeToken = [&tokenList, &index](Token::Type type) -> kj::Maybe<Token&> {
    KJ_ASSERT(index < tokenList.size());
    auto& nextToken = tokenList[index];
    if (nextToken.type != type) {
      return nullptr;
    }
    index++;
    return nextToken;
  };

  const auto tryConsumeModifierToken = [&tryConsumeToken]() -> kj::Maybe<Token&> {
    KJ_IF_MAYBE(token, tryConsumeToken(Token::Type::OTHER_MODIFIER)) {
      return *token;
    }
    return tryConsumeToken(Token::Type::ASTERISK);
  };

  const auto tryConsumeRegexOrWildcardToken = [&tryConsumeToken](kj::Maybe<Token&>& nameToken) {
    auto token = tryConsumeToken(Token::Type::REGEXP);
    if (nameToken == nullptr && token == nullptr) {
      token = tryConsumeToken(Token::Type::ASTERISK);
    }
    return token;
  };

  const auto consumeRequiredToken = [&tryConsumeToken](Token::Type type) -> Token& {
    return JSG_REQUIRE_NONNULL(tryConsumeToken(type),
                                TypeError,
                                kj::str(SYNTAX_ERROR, ": Required token missing."));
  };

  const auto maybeAddPartFromPendingFixedValue =
      [&pendingFixedValue, &encodingCallback, &partList] {
    if (pendingFixedValue.size() == 0) return;
    auto fixedValue = pendingFixedValue.finish();
    pendingFixedValue.reserve(64);
    auto encodedValue = encodingCallback(fixedValue, getCommonStrings().DUMMY_PROTOCOL.asPtr());
    pendingFixedValue.clear();
    partList.add(Part {
      .type = Part::Type::FIXED_TEXT,
      .modifier = Part::Modifier::NONE,
      .value = kj::mv(encodedValue),
    });
  };

  const auto isDuplicateName = [&partList](jsg::UsvStringPtr name) -> bool const {
    return std::any_of(partList.begin(), partList.end(),
                       [&name](Part& part) { return part.name == name; });
  };

  const auto addPart =
      [&maybeAddPartFromPendingFixedValue, &segmentWildcardRegex, &isDuplicateName,
       &partList, &encodingCallback, &pendingFixedValue, &nextNumericName]
       (jsg::UsvString prefix,
        kj::Maybe<Token&> nameToken,
        kj::Maybe<Token&> regexOrWildcardToken,
        jsg::UsvString suffix,
        kj::Maybe<Token&> modifierToken) {
    auto modifier = maybeTokenToModifier(modifierToken);
    if (nameToken == nullptr &&
        regexOrWildcardToken == nullptr &&
        modifier == Part::Modifier::NONE) {
      pendingFixedValue.addAll(prefix);
      return;
    }
    maybeAddPartFromPendingFixedValue();
    if (nameToken == nullptr && regexOrWildcardToken == nullptr) {
      KJ_ASSERT(suffix.size() == 0);
      if (prefix.size() == 0) {
        return;
      }
      auto encodedValue = encodingCallback(prefix, getCommonStrings().DUMMY_PROTOCOL.asPtr());
      partList.add(Part {
        .type = Part::Type::FIXED_TEXT,
        .modifier = modifier,
        .value = kj::mv(encodedValue),
      });
      return;
    }
    auto regexValue = jsg::usv();
    KJ_IF_MAYBE(token, regexOrWildcardToken) {
      if (token->type == Token::Type::ASTERISK) {
        regexValue = jsg::usv(getCommonStrings().FULL_WILDCARD);
      } else {
        regexValue = token->tokenValue();
      }
    } else {
      regexValue = jsg::usv(segmentWildcardRegex);
    }
    auto type = Part::Type::REGEXP;
    if (regexValue == segmentWildcardRegex) {
      type = Part::Type::SEGMENT_WILDCARD;
      regexValue = jsg::usv();
    } else if (regexValue == getCommonStrings().FULL_WILDCARD) {
      type = Part::Type::FULL_WILDCARD;
      regexValue = jsg::usv();
    }
    auto name = jsg::usv();
    KJ_IF_MAYBE(token, nameToken) {
      name = token->tokenValue();
    } else KJ_IF_MAYBE(token, regexOrWildcardToken) {
      name = jsg::usv(kj::toCharSequence(nextNumericName++));
    }

    JSG_REQUIRE(!isDuplicateName(name), TypeError,
                 kj::str(SYNTAX_ERROR, ": Duplicated part names [", name, "]."));
    partList.add(Part {
      .type = type,
      .modifier = modifier,
      .value = kj::mv(regexValue),
      .name = kj::mv(name),
      .prefix = encodingCallback(prefix, nullptr),
      .suffix = encodingCallback(suffix, nullptr),
    });
  };

  const auto consumeText = [&tryConsumeToken]() -> jsg::UsvString {
    jsg::UsvStringBuilder result(64);
    while (true) {
      KJ_IF_MAYBE(token, tryConsumeToken(Token::Type::CHAR)) {
        result.addAll(token->tokenValue());
      } else KJ_IF_MAYBE(token, tryConsumeToken(Token::Type::ESCAPED_CHAR)) {
        result.addAll(token->tokenValue());
      } else {
        break;
      }
    }
    return result.finish();
  };

  while (index < tokenList.size()) {
    auto charToken = tryConsumeToken(Token::Type::CHAR);
    auto nameToken = tryConsumeToken(Token::Type::NAME);
    auto regexOrWildcardToken = tryConsumeRegexOrWildcardToken(nameToken);
    if (nameToken != nullptr || regexOrWildcardToken != nullptr) {
      auto prefix = jsg::usv();
      KJ_IF_MAYBE(token, charToken) {
        prefix = token->tokenValue();
      }
      if (prefix.size() > 0) {
        KJ_IF_MAYBE(codepoint, options.prefixCodePoint) {
          if (prefix.first() != *codepoint) {
            pendingFixedValue.addAll(prefix);
            prefix = jsg::usv();
          }
        } else {
          // If prefix is not empty, and is not the prefixCodePoint
          // (which is can't be if we're here given that there is
          // no prefixCodePoint), when we append prefix to pendingFixedValue,
          // and clear prefix.
          pendingFixedValue.addAll(prefix);
          prefix = jsg::usv();
        }
      }
      maybeAddPartFromPendingFixedValue();
      auto modifierToken = tryConsumeModifierToken();
      addPart(kj::mv(prefix), nameToken, regexOrWildcardToken, jsg::usv(), modifierToken);
      continue;
    }

    auto fixedToken = charToken;
    if (fixedToken == nullptr) {
      fixedToken = tryConsumeToken(Token::Type::ESCAPED_CHAR);
    }
    KJ_IF_MAYBE(token, fixedToken) {
      pendingFixedValue.addAll(token->tokenValue());
      continue;
    }
    KJ_IF_MAYBE(openToken, tryConsumeToken(Token::Type::OPEN)) {
      auto prefix = consumeText();
      auto nameToken = tryConsumeToken(Token::Type::NAME);
      regexOrWildcardToken = tryConsumeRegexOrWildcardToken(nameToken);
      auto suffix = consumeText();
      consumeRequiredToken(Token::Type::CLOSE);
      auto modifierToken = tryConsumeModifierToken();
      addPart(kj::mv(prefix), nameToken, regexOrWildcardToken, kj::mv(suffix), modifierToken);
      continue;
    }
    maybeAddPartFromPendingFixedValue();
    consumeRequiredToken(Token::Type::END);
  }

  return partList.releaseAsArray();
}

RegexAndNameList generateRegularExpressionAndNameList(
    jsg::Lock& js,
    kj::ArrayPtr<Part> partList,
    const CompileOptions& options) {
  // Worst case is that the nameList is equal to partList, although that will almost never
  // be the case, so let's be more conservative in what we reserve.
  kj::Vector<jsg::UsvString> nameList(partList.size() / 2);
  // The reserved size here is a bit arbitrary. We just want to reduce allocations as we build.
  jsg::UsvStringBuilder result(255);
  auto segmentWildcardRegexp = generateSegmentWildcardRegexp(options);
  result.add('^');

  for (auto& part : partList) {
    if (part.type == Part::Type::FIXED_TEXT) {
      auto escaped = escapeRegexString(part.value);
      if (part.modifier == Part::Modifier::NONE) {
        result.addAll(escaped);
      } else {
        result.add('(', '?', ':');
        result.addAll(escaped);
        result.add(')');
        KJ_IF_MAYBE(codepoint, modifierToCodepoint(part.modifier)) {
          result.add(*codepoint);
        }
      }
      continue;
    }
    KJ_ASSERT(part.name.size() > 0);
    nameList.add(jsg::usv(part.name));
    auto regexValue = jsg::usv(part.value);
    if (part.type == Part::Type::SEGMENT_WILDCARD) {
      regexValue = jsg::usv(segmentWildcardRegexp);
    } else if (part.type == Part::Type::FULL_WILDCARD) {
      regexValue = jsg::usv(getCommonStrings().FULL_WILDCARD);
    }
    if (part.prefix.size() == 0 && part.suffix.size() == 0) {
      if (part.modifier == Part::Modifier::NONE || part.modifier == Part::Modifier::OPTIONAL) {
        result.add('(');
        result.addAll(regexValue);
        result.add(')');
        KJ_IF_MAYBE(codepoint, modifierToCodepoint(part.modifier)) {
          result.add(*codepoint);
        }
      } else {
        result.add('(', '(', '?', ':');
        result.addAll(regexValue);
        result.add(')');
        KJ_IF_MAYBE(codepoint, modifierToCodepoint(part.modifier)) {
          result.add(*codepoint);
        }
        result.add(')');
      }
      continue;
    }

    auto escapedPrefix = escapeRegexString(part.prefix);
    auto escapedSuffix = escapeRegexString(part.suffix);

    if (part.modifier == Part::Modifier::NONE || part.modifier == Part::Modifier::OPTIONAL) {
      result.add('(', '?', ':');
      result.addAll(escapedPrefix);
      result.add('(');
      result.addAll(regexValue);
      result.add(')');
      result.addAll(escapedSuffix);
      result.add(')');
      KJ_IF_MAYBE(codepoint, modifierToCodepoint(part.modifier)) {
        result.add(*codepoint);
      }
      continue;
    }
    result.add('(', '?', ':');
    result.addAll(escapedPrefix);
    result.add('(', '(', '?', ':');
    result.addAll(regexValue);
    result.add(')', '(', '?', ':');
    result.addAll(escapedSuffix);
    result.addAll(escapedPrefix);
    result.add('(', '?', ':');
    result.addAll(regexValue);
    result.add(')', ')', '*', ')');
    result.addAll(escapedSuffix);
    result.add(')');
    if (part.modifier == Part::Modifier::ZERO_OR_MORE) {
      result.add('?');
    }
  }
  result.add('$');

  // We're handling the error check ourselves here instead of using jsg::check
  // because the URLPattern spec requires that we throw a TypeError if the
  // regular expression syntax is invalid as opposed to the default SyntaxError
  // that V8 throws.
  return js.tryCatch([&]() {
    auto context = js.v8Context();
    return RegexAndNameList {
      js.v8Ref(jsg::check(v8::RegExp::New(context,
                        v8Str(js.v8Isolate, result.finish()),
                        v8::RegExp::Flags::kUnicode)).As<v8::RegExp>()),
      nameList.releaseAsArray(),
    };
  }, [&](jsg::Value reason) -> RegexAndNameList {
    JSG_FAIL_REQUIRE(TypeError, "Invalid regular expression syntax.");
  });
}

jsg::UsvString generatePatternString(kj::ArrayPtr<Part> partList, const CompileOptions& options) {
  // The reserved size here is a bit arbitrary. The goal is just to reduce
  // allocations while we build.
  jsg::UsvStringBuilder result(255);
  auto segmentWildcardRegexp = generateSegmentWildcardRegexp(options);

  const auto checkNeedsGrouping = [&options](Part& part) {
    if (part.suffix.size() > 0) return true;
    if (part.prefix.size() > 0) {
      KJ_IF_MAYBE(codepoint, options.prefixCodePoint) {
        return part.prefix.first() != *codepoint;
      }
    }
    return false;
  };

  // On each iteration of the for loop, a JavaScript callback is invokved. If a new
  // item is appended to the partList within that function, the loop must pick
  // it up. Using the classic for (;;) syntax here allows for that. However, this does
  // mean that it's possible for a user to trigger an infinite loop here if new items
  // are added to the search params unconditionally on each iteration.
  for (size_t n = 0; n < partList.size(); n++) {
    auto& part = partList[n];
    Part* previousPart = nullptr;
    Part* nextPart = nullptr;
    if (n > 0) previousPart = &partList[n - 1];
    if (n < partList.size() - 1) nextPart = &partList[n + 1];

    if (part.type == Part::Type::FIXED_TEXT) {
      if (part.modifier == Part::Modifier::NONE) {
        result.addAll(escapePatternString(part.value));
        continue;
      }
      result.add('{');
      result.addAll(escapePatternString(part.value));
      result.add('}');
      KJ_IF_MAYBE(codepoint, modifierToCodepoint(part.modifier)) {
        result.add(*codepoint);
      }
      continue;
    }
    auto customName = !isAsciiDigit(part.name.first());
    auto needsGrouping = checkNeedsGrouping(part);

    if (!needsGrouping &&
        part.prefix.empty() &&
        customName &&
        part.type == Part::Type::SEGMENT_WILDCARD &&
        part.modifier == Part::Modifier::NONE &&
        nextPart != nullptr &&
        nextPart->prefix.empty() &&
        nextPart->suffix.empty()) {
      if (nextPart->type == Part::Type::FIXED_TEXT) {
        needsGrouping = !nextPart->name.empty() && isValidCodepoint(nextPart->name.first(), false);
      } else {
        needsGrouping = !nextPart->name.empty() && isAsciiDigit(nextPart->name.first());
      }
    }

    if (!needsGrouping && part.prefix.empty() && previousPart != nullptr) {
      // These additional checks on previousPart have to be separated out from the outer
      // if because in some cases, they may be evaluated before the previousPart != nullptr
      // check.
      if (previousPart->type == Part::Type::FIXED_TEXT &&
          (!previousPart->value.empty() &&
           previousPart->value.last() == options.prefixCodePoint.orDefault(0))) {
        needsGrouping = true;
      }
    }

    KJ_ASSERT(part.name.size() > 0);

    if (needsGrouping) {
      result.add('{');
    }
    result.addAll(escapePatternString(part.prefix));
    if (customName) {
      result.add(':');
      result.addAll(part.name);
    }
    if (part.type == Part::Type::REGEXP) {
      result.add('(');
      result.addAll(part.value);
      result.add(')');
    } else if (part.type == Part::Type::SEGMENT_WILDCARD && !customName) {
      result.add('(');
      result.addAll(segmentWildcardRegexp);
      result.add(')');
    } else if (part.type == Part::Type::FULL_WILDCARD) {
      if (!customName &&
          (
            previousPart == nullptr ||
            previousPart->type == Part::Type::FIXED_TEXT ||
            previousPart->modifier != Part::Modifier::NONE ||
            needsGrouping ||
            !part.prefix.empty())) {
        result.add('*');
      } else {
        result.add('(', '.', '*', ')');
      }
    }
    if (part.type == Part::Type::SEGMENT_WILDCARD &&
        customName &&
        !part.suffix.empty() &&
        isValidCodepoint(part.suffix.first(), false)) {
      result.add('\\');
    }
    result.addAll(escapePatternString(part.suffix));
    if (needsGrouping) {
      result.add('}');
    }
    KJ_IF_MAYBE(codepoint, modifierToCodepoint(part.modifier)) {
      result.add(*codepoint);
    }
  }
  return result.finish();
}

URLPatternComponent compileComponent(
    jsg::Lock& js,
    kj::Maybe<jsg::UsvStringPtr> input,
    EncodingCallback encodingCallback,
    const CompileOptions& options) {
  auto partList = parsePatternString(
      kj::mv(input).orDefault(getCommonStrings().WILDCARD),
      kj::mv(encodingCallback),
      options);
  auto regexAndNameList = generateRegularExpressionAndNameList(js, partList, options);

  return URLPatternComponent {
    .pattern = generatePatternString(partList, options),
    .regex = kj::mv(regexAndNameList.first),
    .nameList = kj::mv(regexAndNameList.second),
  };
}

URLPatternComponent
compileHostnameComponent(jsg::Lock& js,
                         kj::Maybe<jsg::UsvStringPtr> input,
                         const CompileOptions &options = CompileOptions::HOSTNAME) {
  return isIpv6(kj::mv(input).orDefault(getCommonStrings().WILDCARD))
      ? compileComponent(js, input, &canonicalizeIpv6Hostname, options)
      : compileComponent(js, input, &canonicalizeHostname, options);
}

URLPattern::URLPatternInit processPatternInit(
    URLPattern::URLPatternInit& init,
    ProcessPatternInitType type,
    kj::Maybe<jsg::UsvString> protocol = nullptr,
    kj::Maybe<jsg::UsvString> username = nullptr,
    kj::Maybe<jsg::UsvString> password = nullptr,
    kj::Maybe<jsg::UsvString> hostname = nullptr,
    kj::Maybe<jsg::UsvString> port = nullptr,
    kj::Maybe<jsg::UsvString> pathname = nullptr,
    kj::Maybe<jsg::UsvString> search = nullptr,
    kj::Maybe<jsg::UsvString> hash = nullptr) {

  const auto isAbsolutePathname = [&type](jsg::UsvStringPtr str) {
    if (str.size() == 0) return false;
    auto it = str.begin();
    auto c = *it;
    if (c == '/') return true;
    if (type == ProcessPatternInitType::URL) return false;
    if (str.size() < 2) return false;
    return (c == '\\' || c == '{') && *(++it) == '/';
  };

  URLPattern::URLPatternInit result {
    .protocol = kj::mv(protocol),
    .username = kj::mv(username),
    .password = kj::mv(password),
    .hostname = kj::mv(hostname),
    .port = kj::mv(port),
    .pathname = kj::mv(pathname),
    .search = kj::mv(search),
    .hash = kj::mv(hash),
  };
  kj::Maybe<url::UrlRecord> maybeBaseUrl;
  KJ_IF_MAYBE(baseURL, init.baseURL) {
    auto url = JSG_REQUIRE_NONNULL(url::URL::parse(*baseURL), TypeError, "Invalid base URL.");
    result.protocol = jsg::usv(url.scheme);
    result.username = jsg::usv(url.username);
    result.password = jsg::usv(url.password);
    KJ_IF_MAYBE(host, url.host) {
      result.hostname = jsg::usv(*host);
    } else {
      result.hostname = jsg::usv();
    }
    KJ_IF_MAYBE(port, url.port) {
      result.port = jsg::usv(kj::toCharSequence(*port));
    } else {
      result.port = jsg::usv();
    }
    result.pathname = url.getPathname();
    KJ_IF_MAYBE(query, url.query) {
      result.search = jsg::usv(*query);
    } else {
      result.search = jsg::usv();
    }
    KJ_IF_MAYBE(fragment, url.fragment) {
      result.hash = jsg::usv(*fragment);
    } else {
      result.hash = jsg::usv();
    }
    maybeBaseUrl = kj::mv(url);
  }

  KJ_IF_MAYBE(protocol, init.protocol) {
    auto strippedValue = (protocol->size() > 0 && protocol->last() == ':') ?
        protocol->slice(0, protocol->size() - 1) :
        protocol->asPtr();
    result.protocol = type == ProcessPatternInitType::PATTERN ?
        jsg::usv(strippedValue) :
        canonicalizeProtocol(strippedValue, nullptr);
  }
  KJ_IF_MAYBE(username, init.username) {
    result.username = type == ProcessPatternInitType::PATTERN ?
        jsg::usv(*username) :
        canonicalizeUsername(*username, nullptr);
  }
  KJ_IF_MAYBE(password, init.password) {
    result.password = type == ProcessPatternInitType::PATTERN ?
        jsg::usv(*password) :
        canonicalizePassword(*password, nullptr);
  }
  KJ_IF_MAYBE(hostname, init.hostname) {
    result.hostname = type == ProcessPatternInitType::PATTERN ?
        jsg::usv(*hostname) :
        canonicalizeHostname(*hostname, nullptr);
  }
  KJ_IF_MAYBE(port, init.port) {
    result.port = type == ProcessPatternInitType::PATTERN ?
        jsg::usv(*port) :
        canonicalizePort(*port,
                         init.protocol.map([](jsg::UsvString& str) { return str.asPtr(); }));
  }
  KJ_IF_MAYBE(pathname, init.pathname) {
    auto temppath = jsg::usv(*pathname);
    KJ_IF_MAYBE(baseUrl, maybeBaseUrl) {
      if (!isAbsolutePathname(*pathname)) {
        auto baseUrlPath = baseUrl->getPathname();
        KJ_IF_MAYBE(index, baseUrlPath.lastIndexOf('/')) {
          jsg::UsvStringBuilder result(*index + 1 + pathname->size());
          result.addAll(baseUrlPath.slice(0, (*index) + 1));
          result.addAll(*pathname);
          temppath = result.finish();
        }
      }
    }

    if (type != ProcessPatternInitType::PATTERN) {
      KJ_IF_MAYBE(protocol, result.protocol) {
        if (protocol->empty() || url::URL::isSpecialScheme(*protocol)) {
          result.pathname = canonicalizePathname(temppath, nullptr);
        } else {
          result.pathname = canonicalizeOpaquePathname(temppath, nullptr);
        }
      } else {
        // When protocol is not specified it is equivalent to the zero-length string.
        result.pathname = canonicalizePathname(temppath, nullptr);
      }
    } else {
      result.pathname = kj::mv(temppath);
    }
  }
  KJ_IF_MAYBE(search, init.search) {
    auto strippedValue = (search->size() > 0 && search->first() == '?') ?
        jsg::usv(search->slice(1)) :
        jsg::usv(*search);
    if (type == ProcessPatternInitType::PATTERN) {
      result.search = kj::mv(strippedValue);
    } else {
      result.search = canonicalizeSearch(strippedValue, nullptr);
    }
  }
  KJ_IF_MAYBE(hash, init.hash) {
    auto strippedValue = (hash->size() > 0 && hash->first() == '#') ?
        jsg::usv(hash->slice(1)) :
        jsg::usv(*hash);
    if (type == ProcessPatternInitType::PATTERN) {
      result.hash = kj::mv(strippedValue);
    } else {
      result.hash = canonicalizeHash(strippedValue, nullptr);
    }
  }

  return kj::mv(result);
}

URLPattern::URLPatternInit parseConstructorString(
    jsg::Lock& js,
    jsg::UsvStringPtr input,
    jsg::Optional<jsg::UsvString> baseURL) {
  enum class State {
    INIT,
    PROTOCOL,
    AUTHORITY,
    USERNAME,
    PASSWORD,
    HOSTNAME,
    PORT,
    PATHNAME,
    SEARCH,
    HASH,
    DONE,
  };
  URLPattern::URLPatternInit result {
    .baseURL = kj::mv(baseURL)
  };
  State state = State::INIT;
  kj::Array<Token> tokenList = tokenize(input, Token::Policy::LENIENT);
  size_t componentStart = 0;
  size_t tokenIndex = 0;
  size_t tokenIncrement = 0;
  size_t groupDepth = 0;
  size_t ipv6BracketDepth = 0;
  bool protocolMatchesSpecialScheme = false;

  const auto setParserComponentResult = [&result](State state, jsg::UsvStringPtr value) {
    switch (state) {
      case State::PROTOCOL:
        result.protocol = jsg::usv(value);
        return;
      case State::USERNAME:
        result.username = jsg::usv(value);
        return;
      case State::PASSWORD:
        result.password = jsg::usv(value);
        return;
      case State::HOSTNAME:
        result.hostname = jsg::usv(value);
        return;
      case State::PORT:
        result.port = jsg::usv(value);
        return;
      case State::PATHNAME:
        result.pathname = jsg::usv(value);
        return;
      case State::SEARCH:
        result.search = jsg::usv(value);
        return;
      case State::HASH:
        result.hash = jsg::usv(value);
        return;
      default:
        KJ_UNREACHABLE;
    }
  };

  const auto getSafeToken = [&tokenList](size_t index) -> Token& {
    if (index < tokenList.size()) {
      return tokenList[index];
    }
    KJ_ASSERT(tokenList.size() >= 1);
    auto& token = tokenList.back();
    KJ_ASSERT(token.type == Token::Type::END);
    return token;
  };

  const auto makeComponentString =
      [&tokenIndex, &tokenList, &getSafeToken, &input, &componentStart] {
    KJ_ASSERT(tokenIndex < tokenList.size());
    auto& token = tokenList[tokenIndex];
    auto& componentStartToken = getSafeToken(componentStart);
    auto componentStartInputIndex = componentStartToken.index;
    auto endIndex = token.index;
    KJ_ASSERT(componentStartInputIndex <= endIndex);
    return input.slice(componentStartInputIndex, endIndex);
  };

  const auto changeState =
      [&state, &tokenIndex, &componentStart, &tokenIncrement, &setParserComponentResult,
       &makeComponentString](State newState, int skip) {
    if (state != State::INIT &&
        state != State::AUTHORITY &&
        state != State::DONE) {
      setParserComponentResult(state, makeComponentString());
    }
    state = newState;
    tokenIndex += skip;
    componentStart = tokenIndex;
    tokenIncrement = 0;
  };

  const auto rewind = [&tokenIndex, &componentStart, &tokenIncrement] {
    tokenIndex = componentStart;
    tokenIncrement = 0;
  };

  const auto rewindAndSetState = [&rewind, &state](State newState) {
    rewind();
    state = newState;
  };

  const auto isNonSpecialPatternChar =
      [&getSafeToken](int index, uint32_t codepoint) -> auto const {
    auto& token = getSafeToken(index);
    if (token != codepoint) return false;
    return token.type == Token::Type::CHAR ||
           token.type == Token::Type::ESCAPED_CHAR ||
           token.type == Token::Type::INVALID_CHAR;
  };

  const auto isProtocolSuffix = [&isNonSpecialPatternChar, &tokenIndex]() -> auto const {
    return isNonSpecialPatternChar(tokenIndex, ':');
  };

  const auto nextIsAuthoritySlashes = [&isNonSpecialPatternChar, &tokenIndex]() -> auto const {
    return isNonSpecialPatternChar(tokenIndex + 1, '/') &&
           isNonSpecialPatternChar(tokenIndex + 2, '/');
  };

  const auto isIdentityTerminator = [&isNonSpecialPatternChar, &tokenIndex]() -> auto const {
    return isNonSpecialPatternChar(tokenIndex, '@');
  };

  const auto isPasswordPrefix = [&isNonSpecialPatternChar, &tokenIndex]() -> auto const {
    return isNonSpecialPatternChar(tokenIndex, ':');
  };

  const auto isPortPrefix = [&isNonSpecialPatternChar, &tokenIndex]() -> auto const {
    return isNonSpecialPatternChar(tokenIndex, ':');
  };

  const auto isPathnameStart = [&isNonSpecialPatternChar, &tokenIndex]() -> auto const {
    return isNonSpecialPatternChar(tokenIndex, '/');
  };

  const auto isSearchPrefix =
      [&isNonSpecialPatternChar, &tokenIndex, &tokenList, &getSafeToken]() -> auto const {
    if (isNonSpecialPatternChar(tokenIndex, '?')) {
      return true;
    }
    auto& token = tokenList[tokenIndex];
    if (token != '?') {
      return false;
    }
    auto previousIndex = tokenIndex - 1;
    if (previousIndex < 0) {
      return true;
    }
    auto& previousToken = getSafeToken(previousIndex);
    return previousToken.type != Token::Type::NAME &&
           previousToken.type != Token::Type::REGEXP &&
           previousToken.type != Token::Type::CLOSE &&
           previousToken.type != Token::Type::ASTERISK;
  };

  const auto isHashPrefix = [&isNonSpecialPatternChar, &tokenIndex]() -> auto const {
    return isNonSpecialPatternChar(tokenIndex, '#');
  };

  const auto isGroupOpen = [&tokenList, &tokenIndex]() -> auto const {
    return tokenList[tokenIndex].type == Token::Type::OPEN;
  };

  const auto isGroupClose = [&tokenList, &tokenIndex]() -> auto const {
    return tokenList[tokenIndex].type == Token::Type::CLOSE;
  };

  const auto isIPv6Open = [&isNonSpecialPatternChar, &tokenIndex]() -> auto const {
    return isNonSpecialPatternChar(tokenIndex, '[');
  };

  const auto isIPv6Close = [&isNonSpecialPatternChar, &tokenIndex]() -> auto const {
    return isNonSpecialPatternChar(tokenIndex, ']');
  };

  const auto computeMatchesSpecialScheme =
      [&makeComponentString, &protocolMatchesSpecialScheme, &js] {
    auto input = makeComponentString();
    auto component =
        compileComponent(js, input, &canonicalizeProtocol, CompileOptions::DEFAULT);
    protocolMatchesSpecialScheme = protocolComponentMatchesSpecialScheme(js, component);
  };

  while (tokenIndex < tokenList.size()) {
    tokenIncrement = 1;
    auto& token = tokenList[tokenIndex];
    if (token.type == Token::Type::END) {
      if (state == State::INIT) {
        rewind();
        if (isHashPrefix()) {
          changeState(State::HASH, 1);
        } else if (isSearchPrefix()) {
          changeState(State::SEARCH, 1);
          result.hash = jsg::usv();
        } else {
          changeState(State::PATHNAME, 0);
          result.search = jsg::usv();
          result.hash = jsg::usv();
        }
        tokenIndex += tokenIncrement;
        continue;
      }
      if (state == State::AUTHORITY) {
        rewindAndSetState(State::HOSTNAME);
        tokenIndex += tokenIncrement;
        continue;
      }
      changeState(State::DONE, 0);
      break;
    }
    if (isGroupOpen()) {
      groupDepth++;
      tokenIndex += tokenIncrement;
      continue;
    }
    if (groupDepth > 0) {
      if (isGroupClose()) {
        groupDepth--;
      } else {
        tokenIndex += tokenIncrement;
        continue;
      }
    }

    switch (state) {
      case State::INIT: {
        if (isProtocolSuffix()) {
          result.username = jsg::usv();
          result.password = jsg::usv();
          result.hostname = jsg::usv();
          result.port = jsg::usv();
          result.pathname = jsg::usv();
          result.search = jsg::usv();
          result.hash = jsg::usv();
          rewindAndSetState(State::PROTOCOL);
        }
        break;
      }
      case State::PROTOCOL: {
        if (isProtocolSuffix()) {
          computeMatchesSpecialScheme();
          if (protocolMatchesSpecialScheme) {
            result.pathname = jsg::usv(getCommonStrings().EMPTY_PATH);
          }
          State nextState = State::PATHNAME;
          int skip = 1;
          if (nextIsAuthoritySlashes()) {
            nextState = State::AUTHORITY;
            skip = 3;
          } else if (protocolMatchesSpecialScheme) {
            nextState = State::AUTHORITY;
          }
          changeState(nextState, skip);
        }
        break;
      }
      case State::AUTHORITY: {
        if (isIdentityTerminator()) {
          rewindAndSetState(State::USERNAME);
        } else if (isPathnameStart() || isSearchPrefix() || isHashPrefix()) {
          rewindAndSetState(State::HOSTNAME);
        }
        break;
      }
      case State::USERNAME: {
        if (isPasswordPrefix()) {
          changeState(State::PASSWORD, 1);
        } else if (isIdentityTerminator()) {
          changeState(State::HOSTNAME, 1);
        }
        break;
      }
      case State::PASSWORD: {
        if (isIdentityTerminator()) {
          changeState(State::HOSTNAME, 1);
        }
        break;
      }
      case State::HOSTNAME: {
        if (isIPv6Open()) {
          ipv6BracketDepth++;
        } else if (isIPv6Close()) {
          ipv6BracketDepth--;
        } else if (isPortPrefix() && ipv6BracketDepth == 0) {
          changeState(State::PORT, 1);
        } else if (isPathnameStart()) {
          changeState(State::PATHNAME, 0);
        } else if (isSearchPrefix()) {
          changeState(State::SEARCH, 1);
        } else if (isHashPrefix()) {
          changeState(State::HASH, 1);
        }
        break;
      }
      case State::PORT: {
        if (isPathnameStart()) {
          changeState(State::PATHNAME, 0);
        } else if (isSearchPrefix()) {
          changeState(State::SEARCH, 1);
        } else if (isHashPrefix()) {
          changeState(State::HASH, 1);
        }
        break;
      }
      case State::PATHNAME: {
        if (isSearchPrefix()) {
          changeState(State::SEARCH, 1);
        } else if (isHashPrefix()) {
          changeState(State::HASH, 1);
        }
        break;
      }
      case State::SEARCH: {
        if (isHashPrefix()) {
          changeState(State::HASH, 1);
        }
        break;
      }
      case State::HASH: {
        // Nothing to do.
        break;
      }
      case State::DONE: {
        KJ_UNREACHABLE;
      }
    }

    tokenIndex += tokenIncrement;
  }

  JSG_REQUIRE(result.protocol != nullptr ||
               result.baseURL != nullptr,
               TypeError,
               kj::str(SYNTAX_ERROR, ": A relative pattern must have a baseURL."));

  return processPatternInit(result, ProcessPatternInitType::PATTERN);
}

URLPatternComponents init(jsg::Lock& js, URLPattern::URLPatternInit&& init) {
  KJ_IF_MAYBE(protocol, init.protocol) {
    KJ_IF_MAYBE(port, init.port) {
      if (url::URL::isSpecialScheme(*protocol) &&
          isSpecialSchemeDefaultPort(*protocol, *port)) {
        init.port = jsg::usv();
      }
    }
  }

  auto protocolComponent = compileComponent(
      js, init.protocol.map([](jsg::UsvString &str) { return str.asPtr(); }),
      &canonicalizeProtocol, CompileOptions::DEFAULT);

  auto matchesSpecialScheme = protocolComponentMatchesSpecialScheme(js, protocolComponent);

  return URLPatternComponents{
      .protocol = kj::mv(protocolComponent),
      .username = compileComponent(
          js, init.username.map([](jsg::UsvString &str) { return str.asPtr(); }),
          &canonicalizeUsername, CompileOptions::DEFAULT),
      .password = compileComponent(
          js, init.password.map([](jsg::UsvString &str) { return str.asPtr(); }),
          &canonicalizePassword, CompileOptions::DEFAULT),
      .hostname = compileHostnameComponent(
          js, init.hostname.map([](jsg::UsvString &str) { return str.asPtr(); }),
          CompileOptions::DEFAULT),
      .port = compileComponent(
          js, init.port.map([](jsg::UsvString &str) { return str.asPtr(); }),
          &canonicalizePort, CompileOptions::DEFAULT),
      .pathname = compileComponent(
          js, init.pathname.map([](jsg::UsvString &str) { return str.asPtr(); }),
          matchesSpecialScheme ? &canonicalizePathname
                               : &canonicalizeOpaquePathname,
          matchesSpecialScheme ? CompileOptions::PATHNAME : CompileOptions::DEFAULT),
      .search = compileComponent(
          js, init.search.map([](jsg::UsvString &str) { return str.asPtr(); }),
          &canonicalizeSearch, CompileOptions::DEFAULT),
      .hash = compileComponent(
          js, init.hash.map([](jsg::UsvString &str) { return str.asPtr(); }),
          &canonicalizeHash, CompileOptions::DEFAULT),
  };
}

URLPatternComponents init(
    jsg::Lock& js,
    jsg::Optional<URLPattern::URLPatternInput> maybeInput,
    jsg::Optional<jsg::UsvString> baseURL) {
  auto input = kj::mv(maybeInput).orDefault(URLPattern::URLPatternInit());
  KJ_SWITCH_ONEOF(input) {
    KJ_CASE_ONEOF(string, jsg::UsvString) {
      return init(js, parseConstructorString(js, string, kj::mv(baseURL)));
    }
    KJ_CASE_ONEOF(i, URLPattern::URLPatternInit) {
      JSG_REQUIRE(baseURL == nullptr, TypeError, BASEURL_ERROR);
      return init(js, processPatternInit(i, ProcessPatternInitType::PATTERN));
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<URLPattern::URLPatternComponentResult> execRegex(
    jsg::Lock& js,
    URLPatternComponent& component,
    jsg::UsvStringPtr input) {
  using Groups = jsg::Dict<jsg::UsvString, jsg::UsvString>;

  auto context = js.v8Context();

  auto execResult =
      jsg::check(component.regex.getHandle(js)->Exec(
          context, jsg::v8Str(js.v8Isolate, input)));

  if (execResult->IsNullOrUndefined()) {
    return nullptr;
  }

  KJ_ASSERT(execResult->IsArray());
  v8::Local<v8::Array> resultsArray = execResult.As<v8::Array>();
  // Starting at 1 here looks a bit odd but it is intentional. The result of the regex
  // is an array and we're skipping the first element.
  uint32_t index = 1;
  uint32_t length = resultsArray->Length();
  kj::Vector<Groups::Field> fields(length - 1);

  while (index < length) {
    auto value = jsg::check(resultsArray->Get(context, index));
    fields.add(Groups::Field {
      .name = jsg::usv(component.nameList[index - 1]),
      .value = value->IsUndefined() ? jsg::usv() : jsg::usv(js.v8Isolate, value),
    });
    index++;
  }

  return URLPattern::URLPatternComponentResult {
    .input = jsg::usv(input),
    .groups = Groups { .fields = fields.releaseAsArray() },
  };
}

}  // namespace

URLPattern::URLPattern(
    jsg::Lock& js,
    jsg::Optional<URLPatternInput> input,
    jsg::Optional<jsg::UsvString> baseURL)
    : components(init(js, kj::mv(input), kj::mv(baseURL))) {}

void URLPattern::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(components.protocol.regex,
                components.username.regex,
                components.password.regex,
                components.hostname.regex,
                components.port.regex,
                components.pathname.regex,
                components.search.regex,
                components.hash.regex);
}

jsg::UsvStringPtr URLPattern::getProtocol() { return components.protocol.pattern; }
jsg::UsvStringPtr URLPattern::getUsername() { return components.username.pattern; }
jsg::UsvStringPtr URLPattern::getPassword() { return components.password.pattern; }
jsg::UsvStringPtr URLPattern::getHostname() { return components.hostname.pattern; }
jsg::UsvStringPtr URLPattern::getPort() { return components.port.pattern; }
jsg::UsvStringPtr URLPattern::getPathname() { return components.pathname.pattern; }
jsg::UsvStringPtr URLPattern::getSearch() { return components.search.pattern; }
jsg::UsvStringPtr URLPattern::getHash() { return components.hash.pattern; }

jsg::Ref<URLPattern> URLPattern::constructor(
    jsg::Lock& js,
    jsg::Optional<URLPatternInput> input,
    jsg::Optional<jsg::UsvString> baseURL) {
  return jsg::alloc<URLPattern>(js, kj::mv(input), kj::mv(baseURL));
}

bool URLPattern::test(
    jsg::Lock& js,
    jsg::Optional<URLPatternInput> input,
    jsg::Optional<jsg::UsvString> baseURL) {
  return exec(js, kj::mv(input), kj::mv(baseURL)) != nullptr;
}

kj::Maybe<URLPattern::URLPatternResult> URLPattern::exec(
    jsg::Lock& js,
    jsg::Optional<URLPatternInput> maybeInput,
    jsg::Optional<jsg::UsvString> baseURLString) {
  auto input = kj::mv(maybeInput).orDefault(URLPattern::URLPatternInit());
  kj::Vector<URLPattern::URLPatternInput> inputs(2);
  auto protocol = jsg::usv();
  auto username = jsg::usv();
  auto password = jsg::usv();
  auto hostname = jsg::usv();
  auto port = jsg::usv();
  auto pathname = jsg::usv();
  auto search = jsg::usv();
  auto hash = jsg::usv();

  const auto setURLComponents =
      [&protocol, &username, &password, &pathname,
       &hostname, &port, &search, &hash](url::UrlRecord& url) {
    protocol = jsg::usv(url.scheme);
    username = jsg::usv(url.username);
    password = jsg::usv(url.password);
    KJ_IF_MAYBE(host, url.host) {
      hostname = jsg::usv(*host);
    } else {
      hostname = jsg::usv();
    }
    KJ_IF_MAYBE(p, url.port) {
      port = jsg::usv(kj::toCharSequence(*p));
    } else {
      port = jsg::usv();
    }
    pathname = url.getPathname();
    KJ_IF_MAYBE(query, url.query) {
      search = jsg::usv(*query);
    } else {
      search = jsg::usv();
    }
    KJ_IF_MAYBE(fragment, url.fragment) {
      hash = jsg::usv(*fragment);
    } else {
      hash = jsg::usv();
    }
  };

  const auto copyOrEmptyString = [](jsg::Optional<jsg::UsvString>& maybeString) {
    return maybeString.map([](jsg::UsvString& string) {
      return jsg::usv(string);
    }).orDefault(jsg::usv());
  };

  KJ_SWITCH_ONEOF(input) {
    KJ_CASE_ONEOF(string, jsg::UsvString) {
      inputs.add(jsg::usv(string));
      KJ_IF_MAYBE(baseURL, baseURLString) {
        inputs.add(jsg::usv(*baseURL));
        KJ_IF_MAYBE(base, url::URL::parse(*baseURL)) {
          KJ_IF_MAYBE(url, url::URL::parse(string, *base)) {
            setURLComponents(*url);
          } else {
            return nullptr;
          }
        } else {
          return nullptr;
        }
      } else KJ_IF_MAYBE(url, url::URL::parse(string)) {
        setURLComponents(*url);
      } else {
        return nullptr;
      }
    }
    KJ_CASE_ONEOF(i, URLPattern::URLPatternInit) {
      JSG_REQUIRE(baseURLString == nullptr, TypeError, BASEURL_ERROR);
      // The URLPattern specification explicitly says to catch any exceptions
      // thrown here and return null instead of throwing.

      if (!js.tryCatch([&] {
        auto init = processPatternInit(i, ProcessPatternInitType::URL,
            kj::mv(protocol),
            kj::mv(username),
            kj::mv(password),
            kj::mv(hostname),
            kj::mv(port),
            kj::mv(pathname),
            kj::mv(search),
            kj::mv(hash));

        inputs.add(kj::mv(i));
        protocol = copyOrEmptyString(init.protocol);
        username = copyOrEmptyString(init.username);
        password = copyOrEmptyString(init.password);
        hostname = copyOrEmptyString(init.hostname);
        port = copyOrEmptyString(init.port);
        pathname = copyOrEmptyString(init.pathname);
        search = copyOrEmptyString(init.search);
        hash = copyOrEmptyString(init.hash);
        return true;
      }, [&](jsg::Value reason) {
        // JavaScript exceptions that make it to here are just ignored.
        return false;
      })) {
        // If the tryCatch returns false, we're exiting early.
        return nullptr;
      }
    }
  }

  auto protocolExecResult = execRegex(js, components.protocol, protocol);
  auto usernameExecResult = execRegex(js, components.username, username);
  auto passwordExecResult = execRegex(js, components.password, password);
  auto hostnameExecResult = execRegex(js, components.hostname, hostname);
  auto portExecResult = execRegex(js, components.port, port);
  auto pathnameExecResult = execRegex(js, components.pathname, pathname);
  auto searchExecResult = execRegex(js, components.search, search);
  auto hashExecResult = execRegex(js, components.hash, hash);

  if (protocolExecResult == nullptr ||
      usernameExecResult == nullptr ||
      passwordExecResult == nullptr ||
      hostnameExecResult == nullptr ||
      portExecResult == nullptr ||
      pathnameExecResult == nullptr ||
      searchExecResult == nullptr ||
      hashExecResult == nullptr) {
    return nullptr;
  }

  return URLPattern::URLPatternResult {
    .inputs = inputs.releaseAsArray(),
    .protocol = kj::mv(KJ_REQUIRE_NONNULL(protocolExecResult)),
    .username = kj::mv(KJ_REQUIRE_NONNULL(usernameExecResult)),
    .password = kj::mv(KJ_REQUIRE_NONNULL(passwordExecResult)),
    .hostname = kj::mv(KJ_REQUIRE_NONNULL(hostnameExecResult)),
    .port = kj::mv(KJ_REQUIRE_NONNULL(portExecResult)),
    .pathname = kj::mv(KJ_REQUIRE_NONNULL(pathnameExecResult)),
    .search = kj::mv(KJ_REQUIRE_NONNULL(searchExecResult)),
    .hash = kj::mv(KJ_REQUIRE_NONNULL(hashExecResult)),
  };
}

}  // namespace workerd::api
