#include "inspector.h"

#include "jsg.h"

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
class StringViewWithScratch: public v8_inspector::StringView {
public:
  StringViewWithScratch(v8_inspector::StringView text, kj::Array<char16_t>&& scratch)
      : v8_inspector::StringView(text),
        scratch(kj::mv(scratch)) {}

private:
  kj::Array<char16_t> scratch;
};
}  // namespace

v8_inspector::StringView toInspectorStringView(kj::StringPtr text) {
  bool isAscii = true;
  for (char c: text) {
    if (c & 0x80) {
      isAscii = false;
      break;
    }
  }

  if (isAscii) {
    return StringViewWithScratch(
        v8_inspector::StringView(text.asBytes().begin(), text.size()), nullptr);
  } else {
    kj::Array<char16_t> scratch = kj::encodeUtf16(text);
    return StringViewWithScratch(
        v8_inspector::StringView(reinterpret_cast<uint16_t*>(scratch.begin()), scratch.size()),
        kj::mv(scratch));
  }
}

// Inform the inspector of a problem not associated with any particular exception object.
//
// Passes `description` as the exception's detailed message, dummy values for everything else.
void sendExceptionToInspector(
    jsg::Lock& js, v8_inspector::V8Inspector& inspector, kj::StringPtr description) {
  inspector.exceptionThrown(js.v8Context(), v8_inspector::StringView(), v8::Local<v8::Value>(),
      jsg::toInspectorStringView(description), v8_inspector::StringView(), 0, 0, nullptr, 0);
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

  // TODO(cleanup): Move the inspector stuff into a utility within jsg to better
  // encapsulate
  KJ_ASSERT(message);
  v8::Local<v8::Message> msg = message;

  auto context = js.v8Context();

  auto stackTrace = msg->GetStackTrace();

  // The resource name is whatever we set in the Script ctor, e.g. "worker.js".
  auto scriptResourceName = msg->GetScriptResourceName();

  auto lineNumber = msg->GetLineNumber(context).FromMaybe(0);
  auto startColumn = msg->GetStartColumn(context).FromMaybe(0);

  // TODO(soon): EW-2636 Pass a real "script ID" as the last parameter instead of 0. I suspect this
  //   has something to do with the incorrect links in the console when it logs uncaught exceptions.
  inspector.exceptionThrown(context, jsg::toInspectorStringView(kj::mv(source)), exception,
      jsg::toInspectorStringView(kj::str(msg->Get())),
      jsg::toInspectorStringView(kj::str(scriptResourceName)), lineNumber, startColumn,
      inspector.createStackTrace(stackTrace), 0);
}

}  // namespace workerd::jsg
