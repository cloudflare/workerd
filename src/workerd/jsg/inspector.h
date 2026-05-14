#pragma once

#include <v8-inspector.h>

#include <kj/array.h>

namespace kj {
class String;
class StringPtr;
}  // namespace kj

namespace v8_inspector {
class V8Inspector;
class StringView;
}  // namespace v8_inspector

namespace workerd::jsg {

class Lock;
class JsValue;
class JsMessage;

struct StringViewWithScratch {
  StringViewWithScratch(kj::Array<char16_t> scratch, v8_inspector::StringView stringView)
      : scratch(kj::mv(scratch)),
        stringView(kj::mv(stringView)) {}

  kj::Array<char16_t> scratch;
  v8_inspector::StringView stringView;
};

// Converts the given text pointer to a StringView, backed either by the memory of the string
// itself or a scratch buffer, if conversion was needed to handle non-ascii content.
StringViewWithScratch toInspectorStringView(kj::StringPtr text);

// Inform the inspector of a problem not associated with any particular exception object.
//
// Passes `description` as the exception's detailed message, dummy values for everything else.
void sendExceptionToInspector(
    jsg::Lock& js, v8_inspector::V8Inspector& inspector, kj::StringPtr description);

// Inform the inspector of an exception thrown.
//
// Passes `source` as the exception's short message. Reconstructs `message` from `exception` if
// `message` is empty.
void sendExceptionToInspector(jsg::Lock& js,
    v8_inspector::V8Inspector& inspector,
    kj::String source,
    const JsValue& exception,
    JsMessage message);

}  // namespace workerd::jsg

namespace v8_inspector {
kj::String KJ_STRINGIFY(const v8_inspector::StringView& view);
}
