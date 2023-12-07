#pragma once

#include <kj/string.h>
#include <v8-inspector.h>

namespace workerd::jsg {

class Lock;
class JsValue;
class JsMessage;

v8_inspector::StringView toInspectorStringView(kj::StringPtr text);

// Inform the inspector of a problem not associated with any particular exception object.
//
// Passes `description` as the exception's detailed message, dummy values for everything else.
void sendExceptionToInspector(jsg::Lock& js,
                              v8_inspector::V8Inspector& inspector,
                              kj::StringPtr description);

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
