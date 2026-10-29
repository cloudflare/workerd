#include "diagnostics-channel.h"

#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/ser.h>

namespace workerd::api::node {

void DiagnosticsChannelModule::publishToTracer(
    jsg::Lock& js, jsg::JsValue nameValue, jsg::JsValue message) {
  auto& context = IoContext::current();
  KJ_IF_SOME(tracer, context.getWorkerTracer()) {
    auto tracerRef = tracer.clone();
    // Converted here rather than taken as jsg::Name and jsg::Value parameters, so that publish()
    // stays cheap when there is no tracer. The TypeScript side passes a string or a symbol.
    auto name = [&] {
      KJ_IF_SOME(symbol, nameValue.tryCast<jsg::JsSymbol>()) {
        return jsg::Name(js, symbol);
      }
      return jsg::Name(nameValue.toString(js));
    }();
    // clang-format off
    JSG_TRY(js) {
      jsg::Serializer ser(js,
          jsg::Serializer::Options{
            .omitHeader = false,
          });
      ser.write(js, message);
      auto tmp = ser.release();
      JSG_REQUIRE(tmp.sharedArrayBuffers.size() == 0 && tmp.transferredArrayBuffers.size() == 0,
          Error,
          "Diagnostic events cannot be published with SharedArrayBuffer or "
          "transferred ArrayBuffer instances");
      tracerRef->addDiagnosticChannelEvent(
          context.getInvocationSpanContext(), context.now(), name.toString(js), kj::mv(tmp.data));
    } JSG_CATCH(exception KJ_UNUSED) {
      // There was an error serializing the message. This is most likely due to
      // the message containing a non-serializable value. That's ok, we'll just
      // use a fallback message so there's at least a record that an event was
      // published.
      JSG_TRY(js) {
        jsg::Serializer ser(js,
            jsg::Serializer::Options{
              .omitHeader = false,
            });
        ser.write(js, js.str("[[ Diagnostic event was not serializable ]]"_kj));
        auto tmp = ser.release();
        tracerRef->addDiagnosticChannelEvent(
            context.getInvocationSpanContext(), context.now(), name.toString(js), kj::mv(tmp.data));
      } JSG_CATCH(exception) {
        // This should not happen, but if it does, we don't want to crash. We'll
        // log the error and continue.
        LOG_PERIODICALLY(WARNING, "Diagnostic event fallback serialization failed: ", exception.getHandle(js));
        tracerRef->addException(context.getInvocationSpanContext(), context.now(), kj::str("Error"),
            kj::str("Failed to publish diagnostics channel message"), kj::none);
      }
    }
    // clang-format on
  }
}

}  // namespace workerd::api::node
