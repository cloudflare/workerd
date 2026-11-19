#include "inspector.h"

#include "jsg.h"
#include "jsvalue.h"
#include "simdutf.h"
#include "util.h"

#include <v8-inspector.h>

#include <kj/encoding.h>
#include <kj/string.h>

namespace v8_inspector {
kj::String KJ_STRINGIFY(const v8_inspector::StringView& view) {
  if (view.is8Bit()) {
    auto bytes = kj::arrayPtr(view.characters8(), view.length());
    for (auto b: bytes) {
      if (b & 0x80) {
        // Ugh, the bytes aren't just ASCII. We need to re-encode.
        auto utf16 = kj::heapArray<char16_t>(bytes.size());
        for (auto i: kj::indices(bytes)) {
          utf16[i] = bytes[i];
        }
        return kj::decodeUtf16(utf16);
      }
    }

    // Looks like it's all ASCII.
    return kj::str(bytes.asChars());
  } else {
    return kj::decodeUtf16(
        kj::arrayPtr(reinterpret_cast<const char16_t*>(view.characters16()), view.length()));
  }
}
}  // namespace v8_inspector

namespace workerd::jsg {

namespace {

// `value` as kj::str(value) describes it, without a jsg::Lock. kj::none if V8 fails, leaving an
// exception pending.
kj::Maybe<kj::String> describe(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::Local<v8::String> detail;
  if (!toDetailString(isolate, value).ToLocal(&detail)) return kj::none;
  v8::String::Utf8Value utf8(isolate, detail);
  if (*utf8 == nullptr) return kj::str("(couldn't stringify)");
  return kj::str(*utf8);
}

}  // namespace

StringViewWithScratch toInspectorStringView(kj::StringPtr text) {
  bool isAscii = simdutf::validate_ascii(text.begin(), text.size());

  if (isAscii) {
    return StringViewWithScratch(
        nullptr, v8_inspector::StringView(text.asBytes().begin(), text.size()));
  } else {
    kj::Array<char16_t> scratch = kj::encodeUtf16(text);
    auto stringView =
        v8_inspector::StringView(reinterpret_cast<uint16_t*>(scratch.begin()), scratch.size());
    return StringViewWithScratch(kj::mv(scratch), kj::mv(stringView));
  }
}

// Inform the inspector of a problem not associated with any particular exception object.
//
// Passes `description` as the exception's detailed message, dummy values for everything else.
void sendExceptionToInspector(
    jsg::Lock& js, v8_inspector::V8Inspector& inspector, kj::StringPtr description) {
  inspector.exceptionThrown(js.v8Context(), v8_inspector::StringView(), v8::Local<v8::Value>(),
      jsg::toInspectorStringView(description).stringView, v8_inspector::StringView(), 0, 0, nullptr,
      0);
}

void sendExceptionToInspector(jsg::Lock& js,
    v8_inspector::V8Inspector& inspector,
    kj::String source,
    const jsg::JsValue& exception,
    jsg::JsMessage message) {
  if (!message) {
    // This exception didn't come with a Message. This can happen for exceptions delivered via
    // v8::Promise::Catch(), or for exceptions which were tunneled through C++ promises. In the
    // latter case, V8 will create a Message based on the current stack trace, but it won't be
    // super meaningful.
    message = jsg::JsMessage::create(js, jsg::JsValue(exception));
  }

  KJ_ASSERT(message);
  if (!sendExceptionToInspector(js.v8Isolate, inspector, source, exception, message)) {
    throw JsExceptionThrown();
  }
}

bool sendExceptionToInspector(v8::Isolate* isolate,
    v8_inspector::V8Inspector& inspector,
    kj::StringPtr source,
    v8::Local<v8::Value> exception,
    v8::Local<v8::Message> message) {
  v8::HandleScope scope(isolate);
  auto context = isolate->GetCurrentContext();

  auto stackTrace = message->GetStackTrace();

  // The resource name is whatever we set in the Script ctor, e.g. "worker.js".
  KJ_IF_SOME(scriptResourceName, describe(isolate, message->GetScriptResourceName())) {
    KJ_IF_SOME(detailedMessage, describe(isolate, message->Get())) {
      auto lineNumber = message->GetLineNumber(context).FromMaybe(0);
      auto startColumn = message->GetStartColumn(context).FromMaybe(0);

      // TODO(soon): EW-2636 Pass a real "script ID" as the last parameter instead of 0. I suspect
      //   this has something to do with the incorrect links in the console when it logs uncaught
      //   exceptions.
      inspector.exceptionThrown(context, toInspectorStringView(source).stringView, exception,
          toInspectorStringView(detailedMessage).stringView,
          toInspectorStringView(scriptResourceName).stringView, lineNumber, startColumn,
          inspector.createStackTrace(stackTrace), 0);
      return true;
    }
  }
  return false;
}

}  // namespace workerd::jsg
