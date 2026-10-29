// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "tracing.h"

#include <workerd/io/trace.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/setup.h>
#include <workerd/util/thread-scopes.h>

namespace workerd::api::user_tracing {

namespace {

// Approximately how much data we allow to be added to a user span before we start ignoring
// modification requests. This is a soft cap to prevent accidental misuse from unbounded
// memory growth; downstream tail-stream submission may apply additional limits.
constexpr size_t MAX_SPAN_BYTES = 64 * 1024;
constexpr size_t MAX_ATTRIBUTE_ARRAY_ELEMENTS = 512;

size_t estimateAttributeValueSize(const AttributeValue& value) {
  // Approximate size; different encodings will produce different sizes. The goal is to bound
  // accidental overuse, not to be byte-accurate. Array elements are counted individually so that
  // a large array is bounded the same way as a large string.
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(b, bool) {
      return 8;
    }
    KJ_CASE_ONEOF(d, double) {
      return 8;
    }
    KJ_CASE_ONEOF(i, int64_t) {
      return 8;
    }
    KJ_CASE_ONEOF(s, kj::ConstString) {
      return s.size();
    }
    KJ_CASE_ONEOF(arr, tracing::AttributeStringArray) {
      size_t size = arr.size() * sizeof(uint64_t);
      for (auto& s: arr) {
        KJ_IF_SOME(value, s) {
          size += value.size();
        }
      }
      return size;
    }
    KJ_CASE_ONEOF(arr, tracing::AttributeBoolArray) {
      return arr.size() * 8;
    }
    KJ_CASE_ONEOF(arr, tracing::AttributeDoubleArray) {
      return arr.size() * 8;
    }
  }
  KJ_UNREACHABLE;
}

// Element type of a homogeneous attribute array, determined by its first non-nullish element.
enum class ArrayElementType { UNKNOWN, STRING, NUMBER, BOOLEAN };

// Converts a JS array to a homogeneous native array following OpenTelemetry's attribute rules:
// elements must all be strings, all numbers, or all booleans; null and undefined elements are
// preserved as empty values. Returns kj::none for arrays that mix primitive types or contain
// non-primitive elements (objects, nested arrays, bigints, symbols, functions).
kj::Maybe<AttributeValue> arrayToAttributeValue(jsg::Lock& js, jsg::JsArray array) {
  auto type = ArrayElementType::UNKNOWN;
  kj::Vector<kj::Maybe<kj::ConstString>> strings;
  kj::Vector<kj::Maybe<double>> numbers;
  kj::Vector<kj::Maybe<bool>> booleans;

  auto length = array.size();
  for (uint32_t i = 0; i < length; i++) {
    jsg::JsValue element = array.get(js, i);
    if (element.isNullOrUndefined()) {
      strings.add(kj::none);
      numbers.add(kj::none);
      booleans.add(kj::none);
      continue;
    }
    ArrayElementType elementType;
    if (element.isString()) {
      elementType = ArrayElementType::STRING;
    } else if (element.isNumber()) {
      elementType = ArrayElementType::NUMBER;
    } else if (element.isBoolean()) {
      elementType = ArrayElementType::BOOLEAN;
    } else {
      return kj::none;
    }
    if (type == ArrayElementType::UNKNOWN) {
      type = elementType;
    } else if (type != elementType) {
      return kj::none;
    }
    v8::Local<v8::Value> handle = element;
    switch (elementType) {
      case ArrayElementType::STRING:
        strings.add(kj::ConstString(element.toString(js)));
        break;
      case ArrayElementType::NUMBER:
        numbers.add(handle.As<v8::Number>()->Value());
        break;
      case ArrayElementType::BOOLEAN:
        booleans.add(handle->IsTrue());
        break;
      case ArrayElementType::UNKNOWN:
        KJ_UNREACHABLE;
    }
  }

  switch (type) {
    case ArrayElementType::STRING:
      return AttributeValue(strings.releaseAsArray());
    case ArrayElementType::NUMBER:
      return AttributeValue(numbers.releaseAsArray());
    case ArrayElementType::BOOLEAN:
      return AttributeValue(booleans.releaseAsArray());
    case ArrayElementType::UNKNOWN:
      // Empty arrays and arrays containing only empty elements have no observable element type,
      // so the wire format uses the string variant for them.
      return AttributeValue(strings.releaseAsArray());
  }
  KJ_UNREACHABLE;
}

// Converts a JS attribute value to its native representation. Scalars keep the existing JSG
// (Web IDL) coercion via `scalarHandler` for backwards compatibility; arrays are validated
// strictly. Returns kj::none when the attribute should not be recorded.
kj::Maybe<AttributeValue> toAttributeValue(jsg::Lock& js,
    kj::StringPtr key,
    v8::Local<v8::Value> handle,
    const jsg::TypeHandler<TagValue>& scalarHandler) {
  if (handle->IsUndefined()) {
    return kj::none;
  }

  if (handle->IsArray()) {
    auto array = jsg::JsArray(handle.As<v8::Array>());
    if (array.size() > MAX_ATTRIBUTE_ARRAY_ELEMENTS) {
      js.logWarning(kj::str("Ignoring span attribute \"", key, "\": array values may contain at ",
          "most ", MAX_ATTRIBUTE_ARRAY_ELEMENTS, " elements."));
      return kj::none;
    }
    KJ_IF_SOME(value, arrayToAttributeValue(js, array)) {
      return kj::mv(value);
    }
    js.logWarning(kj::str("Ignoring span attribute \"", key,
        "\": array values must be homogeneous arrays of strings, numbers, or booleans."));
    return kj::none;
  }

  KJ_IF_SOME(scalar, scalarHandler.tryUnwrap(js, handle)) {
    KJ_SWITCH_ONEOF(scalar) {
      KJ_CASE_ONEOF(b, bool) {
        return AttributeValue(b);
      }
      KJ_CASE_ONEOF(d, double) {
        return AttributeValue(d);
      }
      KJ_CASE_ONEOF(s, kj::String) {
        return AttributeValue(kj::ConstString(kj::mv(s)));
      }
    }
  }
  return kj::none;
}

// This is a CF semantic for warning conditions surfaced on spans, modeled on OpenTelemetry's exception
// semantic conventions (`exception.type` / `exception.message`).
enum class SpanWarningType {
  SPAN_DATA_LIMIT_EXCEEDED,
};

kj::LiteralStringConst spanWarningTypeName(SpanWarningType type) {
  switch (type) {
    case SpanWarningType::SPAN_DATA_LIMIT_EXCEEDED:
      return "span_data_limit_exceeded"_kjc;
  }
  KJ_UNREACHABLE;
}

// Projects only a copy destined for JS. Native observers, carriers and invocation contexts keep
// their original flags, including an absent decision.
kj::Maybe<tracing::SpanContext> projectJsSpanContext(
    kj::Maybe<tracing::SpanContext> context, kj::Maybe<tracing::TraceFlags> defaultJsFlags) {
  return context.map([&](auto& value) {
    auto flags = value.getTraceFlags() == kj::none ? defaultJsFlags : value.getTraceFlags();
    if (defaultJsFlags != kj::none) {
      flags = flags.map([](auto value) { return tracing::TraceFlags(uint8_t(value) & 0x03); });
    }
    return tracing::SpanContext(
        value.getTraceId(), value.getSpanId(), flags, value.cloneTraceState());
  });
}

TracingSpanContext toTracingSpanContext(kj::Maybe<tracing::SpanContext> context) {
  KJ_IF_SOME(value, context) {
    KJ_IF_SOME(spanId, value.getSpanId()) {
      uint8_t traceFlags = 0;
      KJ_IF_SOME(flags, value.getTraceFlags()) {
        traceFlags = flags;
      }
      return TracingSpanContext{
        .traceId = value.getTraceId().toW3C(),
        .spanId = spanId.toGoString(),
        .traceFlags = traceFlags,
        .traceState = value.cloneTraceState(),
      };
    }
  }

  return TracingSpanContext{
    .traceId = tracing::TraceId::nullId.toW3C(),
    .spanId = tracing::SpanId::nullId.toGoString(),
    .traceFlags = 0,
  };
}

}  // namespace

// ======================================================================================
// SpanState

void SpanState::setAttribute(kj::String key, kj::Maybe<AttributeValue> maybeValue) {
  if (!canRecordAttributes()) {
    return;
  }
  KJ_IF_SOME(value, maybeValue) {
    if (bytesUsed > MAX_SPAN_BYTES) {
      return;
    }
    size_t valueSize = estimateAttributeValueSize(value);
    bytesUsed += key.size() + valueSize;
    if (bytesUsed > MAX_SPAN_BYTES) {
      recordSpanDataLimitError("attribute", key, valueSize);
      return;
    }
    recordAttribute(kj::mv(key), kj::mv(value));
  }
  // If value is kj::none the attribute is left unset (undefined on the JS side).
}

void SpanState::recordException(kj::Maybe<tracing::Exception::Code> code,
    kj::String name,
    kj::String message,
    kj::Maybe<kj::String> stack,
    kj::Maybe<tracing::SpanTimestamp> time) {
  if (!canRecordAttributes()) {
    return;
  }

  size_t valueSize =
      name.size() + message.size() + (time == kj::none ? 0 : sizeof(tracing::SpanTimestamp));
  KJ_IF_SOME(c, code) {
    KJ_SWITCH_ONEOF(c) {
      KJ_CASE_ONEOF(text, kj::String) {
        valueSize += text.size();
      }
      KJ_CASE_ONEOF(_, double) {
        valueSize += sizeof(double);
      }
    }
  }
  KJ_IF_SOME(s, stack) {
    valueSize += s.size();
  }
  bytesUsed += valueSize;
  if (bytesUsed > MAX_SPAN_BYTES) {
    recordSpanDataLimitError("exception", name, valueSize);
    return;
  }
  recordExceptionImpl(kj::mv(code), kj::mv(name), kj::mv(message), kj::mv(stack), time);
}

void SpanState::addEvent(tracing::SpanEvent event) {
  if (!getIsTraced() || eventCount >= 128 || bytesUsed > MAX_SPAN_BYTES) return;
  ++eventCount;
  auto size = event.size();
  bytesUsed += size;
  if (bytesUsed > MAX_SPAN_BYTES) {
    recordSpanDataLimitError("event", event.name, size);
    return;
  }
  recordEvent(kj::mv(event));
}

void SpanState::addLinks(tracing::SpanLinks links) {
  if (!getIsTraced() || linkCount >= 128 || bytesUsed > MAX_SPAN_BYTES) return;
  if (links.links.size() > 128 - linkCount) {
    kj::Vector<tracing::SpanLink> limited;
    for (size_t i = 0; i < 128 - linkCount; ++i) limited.add(kj::mv(links.links[i]));
    links.links = limited.releaseAsArray();
  }
  linkCount += links.links.size();
  auto size = links.size();
  bytesUsed += size;
  if (bytesUsed > MAX_SPAN_BYTES) {
    recordSpanDataLimitError("links", "", size);
    return;
  }
  if (links.links.size() != 0) recordLinks(kj::mv(links));
}

class UserSpanState final: public SpanState {
 public:
  UserSpanState(kj::Rc<workerd::SpanObserver> observer,
      kj::ConstString operationName,
      kj::Maybe<tracing::TraceFlags> defaultJsFlags,
      bool adapterCreated = false)
      : spanContext(projectJsSpanContext(observer->toSpanContext(), defaultJsFlags)),
        sampledOut(observer->isSampledOut()),
        childParent(observer.addRef()),
        adapterCreated(adapterCreated),
        builder(kj::mv(observer), kj::mv(operationName)) {}

  ~UserSpanState() noexcept(false) override {
    end();
  }

  void end() override {
    // Move-assigning a null builder ends the old one (submitting via onClose) and drops the
    // observer reference so subsequent setTag/isObserved calls no-op.
    builder = workerd::SpanBuilder(nullptr);
  }

  bool getIsTraced() override {
    return !sampledOut && builder.isObserved();
  }

  kj::Maybe<tracing::SpanContext> getSpanContext() override {
    KJ_IF_SOME(value, spanContext) {
      return tracing::SpanContext::clone(value);
    }
    return kj::none;
  }

  workerd::SpanParent makeSpanParent() override {
    return childParent.addRef();
  }

  void updateName(kj::ConstString operationName) override {
    builder.setOperationName(kj::mv(operationName));
  }

  void setStatus(tracing::SpanStatus status) override {
    builder.setStatus(kj::mv(status));
  }

  void setEndTimestamp(tracing::SpanTimestamp time) override {
    if (adapterCreated && getIsTraced()) {
      KJ_IF_SOME(observer, childParent.getObserver()) observer.setEndTimestamp(time);
    }
  }

 protected:
  bool canRecordAttributes() override {
    return getIsTraced();
  }

  void recordAttribute(kj::String key, AttributeValue value) override {
    // SpanBuilder::TagInitValue is a superset of AttributeValue, but kj::OneOf does not convert
    // between different alternative sets, so re-dispatch per alternative.
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(b, bool) {
        builder.setTag(kj::ConstString(kj::mv(key)), b, IsCustomTag::YES);
      }
      KJ_CASE_ONEOF(d, double) {
        builder.setTag(kj::ConstString(kj::mv(key)), d, IsCustomTag::YES);
      }
      KJ_CASE_ONEOF(i, int64_t) {
        builder.setTag(kj::ConstString(kj::mv(key)), i, IsCustomTag::YES);
      }
      KJ_CASE_ONEOF(s, kj::ConstString) {
        builder.setTag(kj::ConstString(kj::mv(key)), kj::mv(s), IsCustomTag::YES);
      }
      KJ_CASE_ONEOF(arr, tracing::AttributeStringArray) {
        builder.setTag(kj::ConstString(kj::mv(key)), kj::mv(arr), IsCustomTag::YES);
      }
      KJ_CASE_ONEOF(arr, tracing::AttributeBoolArray) {
        builder.setTag(kj::ConstString(kj::mv(key)), kj::mv(arr), IsCustomTag::YES);
      }
      KJ_CASE_ONEOF(arr, tracing::AttributeDoubleArray) {
        builder.setTag(kj::ConstString(kj::mv(key)), kj::mv(arr), IsCustomTag::YES);
      }
    }
  }

  void recordExceptionImpl(kj::Maybe<tracing::Exception::Code> code,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack,
      kj::Maybe<tracing::SpanTimestamp> time) override {
    KJ_IF_SOME(explicitTime, time) {
      KJ_IF_SOME(observer, childParent.getObserver()) {
        tracing::Exception exception(
            observer.getTime(), kj::mv(name), kj::mv(message), kj::mv(stack), kj::mv(code));
        exception.time = explicitTime;
        observer.onExceptionWithTimestamp(kj::mv(exception));
      }
    } else {
      builder.recordException(kj::mv(code), kj::mv(name), kj::mv(message), kj::mv(stack));
    }
  }

  void recordEvent(tracing::SpanEvent event) override {
    KJ_IF_SOME(observer, childParent.getObserver()) observer.onEvent(kj::mv(event));
  }

  void recordLinks(tracing::SpanLinks links) override {
    KJ_IF_SOME(observer, childParent.getObserver()) observer.onLinks(kj::mv(links));
  }

  void recordSpanDataLimitError(
      kj::StringPtr itemKind, kj::StringPtr name, size_t valueSize) override {
    if (!builder.isObserved()) {
      return;
    }
    kj::String shortName;
    if (name.size() > 64) {
      shortName = kj::str("\"", name.slice(0, 64), "...\" (key length ", name.size(), ")");
    } else {
      shortName = kj::str("\"", name, "\"");
    }
    auto message = kj::ConstString(kj::str("exceeded span data limit while trying to record ",
        itemKind, " ", shortName, " of size ", valueSize));
    builder.setTag("cloudflare.warning.type"_kjc,
        spanWarningTypeName(SpanWarningType::SPAN_DATA_LIMIT_EXCEEDED));
    builder.setTag("cloudflare.warning.message"_kjc, kj::mv(message));
  }

 private:
  kj::Maybe<tracing::SpanContext> spanContext;
  const bool sampledOut;
  // Unlike `builder`, this keeps the observer after end() so the span can still parent spans that
  // are created later, as OpenTelemetry allows.
  workerd::SpanParent childParent;
  bool adapterCreated;
  workerd::SpanBuilder builder;
};

class InvocationSpanState final: public SpanState {
 public:
  InvocationSpanState(workerd::SpanParent parent,
      kj::Maybe<kj::Own<BaseTracer::WeakRef>> tracer,
      kj::Maybe<tracing::InvocationSpanContext> context,
      kj::Maybe<tracing::TraceFlags> defaultJsFlags)
      : parent(kj::mv(parent)),
        tracer(kj::mv(tracer)),
        context(kj::mv(context)),
        defaultJsFlags(defaultJsFlags) {}

  // We should treat span.end() as a no-op for the invocation span because
  // this lifecycle is controlled by the runtime.
  void end() override {}

  bool getIsTraced() override {
    KJ_IF_SOME(observer, parent.getObserver()) {
      if (observer.isSampledOut()) return false;
    }
    if (context != kj::none && parent.isObserved()) {
      KJ_IF_SOME(value, tracer) {
        return value->runIfAlive([](BaseTracer&) {});
      }
    }
    return false;
  }

  kj::Maybe<tracing::SpanContext> getSpanContext() override {
    KJ_IF_SOME(value, context) {
      return projectJsSpanContext(tracing::SpanContext(value.getTraceId(), value.getSpanId(),
                                      value.getTraceFlags(), value.cloneTraceState()),
          defaultJsFlags);
    }
    return kj::none;
  }

  workerd::SpanParent makeSpanParent() override {
    return parent.addRef();
  }

  void updateName(kj::ConstString operationName) override {
    if (!getIsTraced()) return;
    KJ_IF_SOME(valueContext, context) {
      KJ_IF_SOME(valueTracer, tracer) {
        valueTracer->runIfAlive([&](BaseTracer& tracer) {
          tracer.addSpanUpdate(
              valueContext.getSpanId(), tracing::SpanUpdate(kj::mv(operationName)));
        });
      }
    }
  }

  void setStatus(tracing::SpanStatus status) override {
    if (!getIsTraced()) return;
    KJ_IF_SOME(valueContext, context) {
      KJ_IF_SOME(valueTracer, tracer) {
        valueTracer->runIfAlive([&](BaseTracer& tracer) {
          tracer.addSpanUpdate(valueContext.getSpanId(), tracing::SpanUpdate(kj::mv(status)));
        });
      }
    }
  }

 protected:
  bool canRecordAttributes() override {
    return getIsTraced();
  }

  void recordAttribute(kj::String key, AttributeValue value) override {
    KJ_IF_SOME(valueContext, context) {
      KJ_IF_SOME(valueTracer, tracer) {
        valueTracer->runIfAlive([&](BaseTracer& tracer) {
          tracer.addSpanAttribute(valueContext, kj::ConstString(kj::mv(key)), kj::mv(value));
        });
      }
    }
  }

  void recordEvent(tracing::SpanEvent event) override {
    KJ_IF_SOME(valueContext, context) {
      KJ_IF_SOME(valueTracer, tracer) {
        valueTracer->runIfAlive([&](BaseTracer& tracer) {
          tracer.addSpanEvent(valueContext.getSpanId(), kj::mv(event));
        });
      }
    }
  }

  void recordLinks(tracing::SpanLinks links) override {
    KJ_IF_SOME(valueContext, context) {
      KJ_IF_SOME(valueTracer, tracer) {
        valueTracer->runIfAlive([&](BaseTracer& tracer) {
          tracer.addSpanLinks(valueContext.getSpanId(), kj::mv(links));
        });
      }
    }
  }

  void recordExceptionImpl(kj::Maybe<tracing::Exception::Code> code,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack,
      kj::Maybe<tracing::SpanTimestamp> time) override {
    KJ_IF_SOME(valueContext, context) {
      KJ_IF_SOME(observer, parent.getObserver()) {
        auto timestamp = observer.getTime();
        KJ_IF_SOME(valueTracer, tracer) {
          valueTracer->runIfAlive([&](BaseTracer& tracer) {
            KJ_IF_SOME(explicitTime, time) {
              tracing::Exception exception(
                  timestamp, kj::mv(name), kj::mv(message), kj::mv(stack), kj::mv(code));
              exception.time = explicitTime;
              tracer.addSpanExceptionWithTimestamp(valueContext.getSpanId(), kj::mv(exception));
            } else {
              tracer.addSpanException(valueContext.getSpanId(), timestamp, kj::mv(code),
                  kj::mv(name), kj::mv(message), kj::mv(stack));
            }
          });
        }
      }
    }
  }

 private:
  workerd::SpanParent parent;
  kj::Maybe<kj::Own<BaseTracer::WeakRef>> tracer;
  kj::Maybe<tracing::InvocationSpanContext> context;
  kj::Maybe<tracing::TraceFlags> defaultJsFlags;
};

class NoopSpanState final: public SpanState {
 public:
  void end() override {}

  bool getIsTraced() override {
    return false;
  }

  kj::Maybe<tracing::SpanContext> getSpanContext() override {
    return kj::none;
  }

  workerd::SpanParent makeSpanParent() override {
    return workerd::SpanParent(nullptr);
  }

  void updateName(kj::ConstString) override {}

  void setStatus(tracing::SpanStatus) override {}

 protected:
  bool canRecordAttributes() override {
    return false;
  }

  void recordAttribute(kj::String, AttributeValue) override {}

  void recordExceptionImpl(kj::Maybe<tracing::Exception::Code>,
      kj::String,
      kj::String,
      kj::Maybe<kj::String>,
      kj::Maybe<tracing::SpanTimestamp>) override {}
};

// ======================================================================================
// Span

Span::Span(kj::OneOf<kj::Own<SpanState>, IoOwn<SpanState>> state): state(kj::mv(state)) {}

bool Span::getIsTraced() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(s, kj::Own<SpanState>) {
      return s->getIsTraced();
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanState>) {
      return s->getIsTraced();
    }
  }
  KJ_UNREACHABLE;
}

jsg::JsObject Span::spanContextForJs(jsg::Lock& js) {
  auto value = spanContext();
  auto result = js.obj();
  result.set(js, "traceId", js.str(value.traceId));
  result.set(js, "spanId", js.str(value.spanId));
  result.set(js, "traceFlags", js.num(value.traceFlags));
  if (IoContext::hasCurrent() && IoContext::current().isOpentelemetryApiEnabled()) {
    KJ_IF_SOME(state, value.traceState) result.set(js, "traceState", js.str(state));
  }
  return result;
}

TracingSpanContext Span::spanContext() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(s, kj::Own<SpanState>) {
      return toTracingSpanContext(s->getSpanContext());
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanState>) {
      return toTracingSpanContext(s->getSpanContext());
    }
  }
  KJ_UNREACHABLE;
}

jsg::Ref<Span> Span::setAttribute(jsg::Lock& js,
    kj::String key,
    jsg::Optional<jsg::Value> value,
    const jsg::TypeHandler<TagValue>& scalarHandler) {
  // Convert regardless of whether the span is traced so that invalid values behave the same way
  // (coercion, warnings, TypeErrors) on traced and untraced spans.
  kj::Maybe<AttributeValue> maybeValue;
  KJ_IF_SOME(v, value) {
    maybeValue = toAttributeValue(js, key, v.getHandle(js), scalarHandler);
  }
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(s, kj::Own<SpanState>) {
      s->setAttribute(kj::mv(key), kj::mv(maybeValue));
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanState>) {
      s->setAttribute(kj::mv(key), kj::mv(maybeValue));
    }
  }
  return JSG_THIS;
}

jsg::Ref<Span> Span::setAttributes(jsg::Lock& js,
    jsg::Dict<jsg::Optional<jsg::Value>> attributes,
    const jsg::TypeHandler<TagValue>& scalarHandler) {
  for (auto& field: attributes.fields) {
    setAttribute(js, kj::mv(field.name), kj::mv(field.value), scalarHandler);
  }
  return JSG_THIS;
}

void Span::recordException(
    jsg::Lock& js, jsg::Value exception, const jsg::TypeHandler<ExceptionData>& exceptionHandler) {
  recordExceptionWithTimestamp(js, kj::mv(exception), exceptionHandler, kj::none);
}

void Span::recordExceptionWithTimestamp(jsg::Lock& js,
    jsg::Value exception,
    const jsg::TypeHandler<ExceptionData>& exceptionHandler,
    kj::Maybe<tracing::SpanTimestamp> time) {
  if (!getIsTraced()) {
    return;
  }

  kj::String name;
  kj::String message;
  kj::Maybe<kj::String> stack;
  kj::Maybe<tracing::Exception::Code> code;
  auto handle = exception.getHandle(js);
  if (handle->IsString()) {
    message = jsg::JsValue(handle).toString(js);
  } else if (handle->IsObject()) {
    auto data = KJ_REQUIRE_NONNULL(exceptionHandler.tryUnwrap(js, handle));
    bool hasRequiredField =
        data.code != kj::none || data.name != kj::none || data.message != kj::none;
    if (!hasRequiredField) {
      return;
    }
    KJ_IF_SOME(c, data.code) {
      KJ_SWITCH_ONEOF(c) {
        KJ_CASE_ONEOF(s, kj::String) {
          code = kj::mv(s);
        }
        KJ_CASE_ONEOF(n, double) {
          code = n;
        }
      }
    }
    KJ_IF_SOME(n, data.name) {
      name = kj::mv(n);
    }
    KJ_IF_SOME(m, data.message) {
      message = kj::mv(m);
    }
    KJ_IF_SOME(s, data.stack) {
      stack = kj::mv(s);
    }
  } else {
    return;
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(s, kj::Own<SpanState>) {
      s->recordException(kj::mv(code), kj::mv(name), kj::mv(message), kj::mv(stack), time);
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanState>) {
      s->recordException(kj::mv(code), kj::mv(name), kj::mv(message), kj::mv(stack), time);
    }
  }
}

jsg::Ref<Span> Span::updateName(jsg::Lock& js, kj::String operationName) {
  if (operationName.size() > MAX_USER_OPERATION_NAME_BYTES) {
    operationName = kj::str(operationName.first(MAX_USER_OPERATION_NAME_BYTES));
  }
  auto name = kj::ConstString(kj::mv(operationName));
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(s, kj::Own<SpanState>) {
      s->updateName(kj::mv(name));
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanState>) {
      s->updateName(kj::mv(name));
    }
  }
  return JSG_THIS;
}

jsg::Ref<Span> Span::setStatus(jsg::Lock& js, TracingSpanStatus status) {
  tracing::SpanStatusCode code;
  if (status.code == "unset") {
    code = tracing::SpanStatusCode::UNSET;
  } else if (status.code == "ok") {
    code = tracing::SpanStatusCode::OK;
  } else if (status.code == "error") {
    code = tracing::SpanStatusCode::ERROR;
  } else {
    JSG_FAIL_REQUIRE(TypeError, "Span status code must be 'unset', 'ok', or 'error'.");
  }

  kj::Maybe<kj::ConstString> message;
  if (code == tracing::SpanStatusCode::ERROR) {
    KJ_IF_SOME(value, status.message) {
      if (value.size() > MAX_SPAN_BYTES) {
        value = kj::str(value.first(MAX_SPAN_BYTES));
      }
      message = kj::ConstString(kj::mv(value));
    }
  }
  tracing::SpanStatus internalStatus(code, kj::mv(message));
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(s, kj::Own<SpanState>) {
      s->setStatus(kj::mv(internalStatus));
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanState>) {
      s->setStatus(kj::mv(internalStatus));
    }
  }
  return JSG_THIS;
}

void Span::end() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(s, kj::Own<SpanState>) {
      s->end();
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanState>) {
      s->end();
    }
  }
}

workerd::SpanParent Span::makeSpanParent() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(s, kj::Own<SpanState>) {
      return s->makeSpanParent();
    }
    KJ_CASE_ONEOF(s, IoOwn<SpanState>) {
      return s->makeSpanParent();
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd::api::user_tracing

// ======================================================================================
// Tracing

namespace workerd::api {

jsg::Optional<jsg::Value> TracingContextVariable::get(jsg::Lock& js) {
  KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(js)) {
    KJ_IF_SOME(value, frame.get(*key)) {
      return jsg::Value(js.v8Isolate, value.getHandle(js));
    }
  }
  return kj::none;
}

jsg::JsRef<jsg::JsValue> TracingContextVariable::run(
    jsg::Lock& js, jsg::Value value, jsg::Function<jsg::JsRef<jsg::JsValue>()> callback) {
  jsg::AsyncContextFrame::StorageScope scope(js, key.addRef(), kj::mv(value));
  return callback(js);
}

jsg::Ref<TracingContextVariable> TracingContext::createAsyncContextVariable(jsg::Lock& js) {
  return js.alloc<TracingContextVariable>();
}

jsg::Optional<user_tracing::TracingSpanContext> TracingContext::getInvocationSpanContext(
    jsg::Lock& js) {
  if (!IoContext::hasCurrent()) return kj::none;
  auto& io = IoContext::current();
  KJ_IF_SOME(root, io.getUserTraceInvocationIdentity()) {
    return user_tracing::toTracingSpanContext(
        user_tracing::projectJsSpanContext(tracing::SpanContext(root.getTraceId(), root.getSpanId(),
                                               root.getTraceFlags(), root.cloneTraceState()),
            io.getDefaultJsTraceFlags()));
  }
  return kj::none;
}

bool TracingContext::hasTracer(jsg::Lock& js) {
  return IoContext::hasCurrent() && IoContext::current().getWorkerTracer() != kj::none;
}

kj::StringPtr TracingContext::getSpanCreationMode(jsg::Lock& js) {
  if (!IoContext::hasCurrent()) return "untraced"_kj;
  auto& io = IoContext::current();
  if (io.getWorkerTracer() == kj::none) return "untraced"_kj;
  KJ_IF_SOME(flags, io.getUserTraceInvocationFlags()) {
    if (!flags.isSampled()) return "unsampled"_kj;
  }
  auto parent = io.getCurrentUserTraceSpan();
  if (!parent.isObserved()) return "untraced"_kj;
  KJ_IF_SOME(identity, parent.toSpanContext()) {
    KJ_IF_SOME(flags, identity.getTraceFlags()) {
      if (!flags.isSampled()) return "unsampled"_kj;
    }
  }
  return "recording"_kj;
}

namespace {
kj::StringPtr rootCreationMode() {
  if (!IoContext::hasCurrent()) return "untraced"_kj;
  auto& io = IoContext::current();
  if (!io.isOpentelemetryApiEnabled() || io.getWorkerTracer() == kj::none ||
      !io.isRootUserTraceObserved())
    return "untraced"_kj;
  bool attached = !KJ_ASSERT_NONNULL(io.getWorkerTracer()).supportsDetachedRoots();
  KJ_IF_SOME(flags, io.getUserTraceInvocationFlags()) {
    if (!flags.isSampled()) return attached ? "attached-unsampled"_kj : "unsampled"_kj;
  }
  return attached ? "attached"_kj : "recording"_kj;
}
}  // namespace

kj::StringPtr TracingContext::getRootCreationMode(jsg::Lock& js) {
  return rootCreationMode();
}

kj::String TracingContext::createRootTraceId(jsg::Lock& js) {
  auto& io = IoContext::current();
  return KJ_ASSERT_NONNULL(io.getWorkerTracer()).makeDetachedTraceId(io.getEntropySource()).toW3C();
}

jsg::JsRef<jsg::JsValue> TracingContext::withActiveSpanContext(jsg::Lock& js,
    jsg::Optional<user_tracing::TracingSpanContext> context,
    jsg::Function<jsg::JsRef<jsg::JsValue>()> callback,
    jsg::Optional<bool> suppress) {
  // Clear the active JS span before installing the supplied parent (or invocation fallback).
  jsg::AsyncContextFrame::StorageScope activeScope(js,
      jsg::IsolateBase::from(js.v8Isolate).getActiveSpanAsyncContextKey(),
      js.v8Ref<v8::Value>(v8::Undefined(js.v8Isolate)));
  if (!IoContext::hasCurrent()) return callback(js);
  auto& io = IoContext::current();
  SpanParent parent(nullptr);
  if (!suppress.orDefault(false) &&
      (io.getWorkerTracer() != kj::none || io.isOpentelemetryApiEnabled())) {
    kj::Maybe<tracing::SpanContext> parsed;
    KJ_IF_SOME(value, context) {
      // Remote parents carry a sampling bit; same-invocation native flags are restored below.
      char flags[] = {'0', (value.traceFlags & 1) ? '1' : '0'};
      parsed = tracing::SpanContext::tryFromTraceparent(
          kj::str("00-", value.traceId, "-", value.spanId, "-", kj::ArrayPtr(flags)));
      KJ_IF_SOME(identity, parsed) {
        if (io.isOpentelemetryApiEnabled()) {
          KJ_IF_SOME(state, value.traceState) {
            identity.setTraceState(tracing::SpanContext::parseTracestate(state));
          }
        }
        KJ_IF_SOME(invocation, io.getUserTraceInvocationContext()) {
          bool minted = io.getWorkerTracer()
                            .map([&](auto& tracer) {
            return tracer.isDetachedTraceId(identity.getTraceId());
          }).orDefault(false);
          if (io.isOpentelemetryApiEnabled() &&
              (identity.getTraceId() == invocation.getTraceId() || minted ||
                  invocation.getTraceFlags().map([](auto flags) {
            return !flags.isSampled();
          }).orDefault(false))) {
            // Invocation and locally minted trace identities inherit the owner's native decision,
            // including absent flags and reserved bits. Explicitly unsampled invocations also
            // override foreign sampled parents. Projected JS flags are not new decisions.
            identity = tracing::SpanContext(identity.getTraceId(), identity.getSpanId(),
                invocation.getTraceFlags(), identity.cloneTraceState());
          }
          // A projected identity cached in module/DO memory and replayed in a different
          // invocation trace is indistinguishable from a remote parent with explicit flags.
        }
      }
    }
    if (parsed == kj::none) {
      KJ_IF_SOME(root, io.getUserTraceInvocationIdentity()) {
        // Bare ROOT_CONTEXT affects adapter parenting, not runtime instrumentation: without
        // an active OTel span, native spans remain attached to the owning invocation.
        bool attached = false;
        if (io.getWorkerTracer() != kj::none) {
          if (io.isOpentelemetryApiEnabled() &&
              !root.getTraceFlags().map([](auto flags) {
            return !flags.isSampled();
          }).orDefault(false)) {
            // Reuse the owner's submitter and its predictable span-ID sequence.
            parent = io.getUserTraceInvocationRoot();
            attached = parent.isObserved();
          }
        }
        if (!attached)
          parsed = tracing::SpanContext(
              root.getTraceId(), root.getSpanId(), root.getTraceFlags(), root.cloneTraceState());
      }
    }
    KJ_IF_SOME(value, parsed) {
      if (value.getTraceFlags().map([](auto flags) {
        return !flags.isSampled();
      }).orDefault(false)) {
        parent = SpanParent(kj::rc<PropagationOnlySpanObserver>(kj::mv(value)));
      } else {
        KJ_IF_SOME(tracer, io.getWorkerTracer()) {
          parent = tracer.makeUserSpanParent(tracing::SpanContext::clone(value));
        }
        if (!parent.isObserved()) {
          parent = SpanParent(kj::rc<PropagationOnlySpanObserver>(kj::mv(value)));
        }
      }
    }
  }
  auto scope = io.makeUserAsyncTraceScope(io.getCurrentLock(), kj::mv(parent));
  return callback(js);
}

namespace {

enum class SpanEndMode { AUTO_END, MANUAL_END };

struct CreatedSpan {
  jsg::Ref<user_tracing::Span> span;
  kj::Maybe<SpanParent> childSpanForAsyncContext;
};

CreatedSpan createSpan(jsg::Lock& js,
    kj::String operationName,
    kj::Maybe<tracing::SpanMetadata> metadata = kj::none,
    DetachedRoot detached = DetachedRoot::NO) {
  // We use qualified `user_tracing::Span` / `user_tracing::SpanState` throughout because an
  // unqualified `Span` in this namespace resolves to workerd::Span (the runtime span struct),
  // which is a different type.

  // Cap operation name length at the API boundary so every downstream submitter sees the
  // truncated value.
  if (operationName.size() > user_tracing::MAX_USER_OPERATION_NAME_BYTES) {
    operationName = kj::str(operationName.first(user_tracing::MAX_USER_OPERATION_NAME_BYTES));
  }

  kj::Own<user_tracing::SpanState> state;
  kj::Maybe<SpanParent> childSpanForAsyncContext;
  bool hasIoContext = IoContext::hasCurrent();

  if (hasIoContext) {
    auto& context = IoContext::current();
    SpanParent parent = context.getCurrentUserTraceSpan();
    if (detached == DetachedRoot::YES) {
      auto mode = rootCreationMode();
      parent = SpanParent(nullptr);
      if (mode == "attached"_kj || mode == "attached-unsampled"_kj) {
        KJ_IF_SOME(invocation, context.getUserTraceInvocationContext()) {
          if (invocation.getTraceFlags().map([](auto flags) {
            return !flags.isSampled();
          }).orDefault(false)) {
            parent = SpanParent(kj::rc<PropagationOnlySpanObserver>(
                tracing::SpanContext(invocation.getTraceId(), invocation.getSpanId(),
                    invocation.getTraceFlags(), invocation.cloneTraceState())));
          } else {
            parent = context.getUserTraceInvocationRoot();
          }
        }
      }
      // Decide before generating entropy. A requested root never changes native sampling.
      if (mode == "recording"_kj) {
        KJ_IF_SOME(invocation, context.getUserTraceInvocationContext()) {
          KJ_IF_SOME(tracer, context.getWorkerTracer()) {
            parent = tracer.makeUserSpanParent(
                tracing::SpanContext(tracer.makeDetachedTraceId(context.getEntropySource()),
                    kj::none, invocation.getTraceFlags()),
                DetachedRoot::YES);
          }
        }
      }
    }

    if (parent.isObserved()) {
      KJ_IF_SOME(observer, parent.getObserver()) {
        // newChildFromUserCode (vs newChild) signals user-origin to the submitter so it can
        // skip the operation-name allowlist that gates runtime spans.
        auto childObserver = observer.inheritTraceProvenance(observer.newChildFromUserCode());
        bool adapterCreated = metadata != kj::none;
        KJ_IF_SOME(value, metadata) {
          childObserver->setOpenMetadata(kj::mv(value));
        }
        state = kj::refcounted<user_tracing::UserSpanState>(kj::mv(childObserver),
            kj::ConstString(kj::heapString(operationName)), context.getDefaultJsTraceFlags(),
            adapterCreated);
        // Capture a SpanParent for the child so startActiveSpan() / enterSpan() can push it onto
        // the AsyncContextFrame. Safe to carry across the request boundary thanks to
        // BaseTracer::WeakRef in the submitter - stale parents cannot pin the tracer.
        childSpanForAsyncContext = state->makeSpanParent();
      } else {
        state = kj::refcounted<user_tracing::NoopSpanState>();
      }
    } else {
      state = kj::refcounted<user_tracing::NoopSpanState>();
    }
  } else {
    // No IoContext: create a no-op span.
    state = kj::refcounted<user_tracing::NoopSpanState>();
  }

  // Wrap state in IoOwn (when inside an IoContext) so destruction funnels through the
  // IoContext's delete queue and cannot cross threads. Outside an IoContext, fall back to
  // kj::Own; tracing without an IoContext is a no-op tracing-wise.
  auto span = [&]() -> jsg::Ref<user_tracing::Span> {
    if (hasIoContext) {
      auto ownedState = IoContext::current().addObject(kj::mv(state));
      return js.alloc<user_tracing::Span>(kj::mv(ownedState));
    }
    return js.alloc<user_tracing::Span>(kj::mv(state));
  }();

  return CreatedSpan{
    .span = kj::mv(span), .childSpanForAsyncContext = kj::mv(childSpanForAsyncContext)};
}

// Runs `func` with `span` as the active span. The JS span object is stored for getActiveSpan(), and
// `parent`, if any, becomes the current user trace span that built-in instrumentation and new user
// spans attach to (via IoContext::getCurrentUserTraceSpan()). The StorageScope RAII objects restore
// the prior async-context storage on scope exit; any async continuations captured during `func`
// will already have snapshotted the new frame and will see `span` as "current".
template <typename Func>
v8::Local<v8::Value> runWithActiveSpan(jsg::Lock& js,
    jsg::Ref<user_tracing::Span>& span,
    kj::Maybe<SpanParent> parent,
    const jsg::TypeHandler<jsg::Ref<user_tracing::Span>>& spanHandler,
    Func&& func) {
  auto runWithJsSpan = [&]() -> v8::Local<v8::Value> {
    auto activeSpan = spanHandler.wrap(js, span.addRef());
    jsg::AsyncContextFrame::StorageScope activeSpanScope(js,
        jsg::IsolateBase::from(js.v8Isolate).getActiveSpanAsyncContextKey(), js.v8Ref(activeSpan));
    return func();
  };

  KJ_IF_SOME(p, kj::mv(parent)) {
    auto& context = IoContext::current();
    jsg::AsyncContextFrame::StorageScope traceScope =
        context.makeUserAsyncTraceScope(context.getCurrentLock(), kj::mv(p));
    return runWithJsSpan();
  } else {
    return runWithJsSpan();
  }
}

// Calls callback(...args) with the global object as `this`.
v8::Local<v8::Value> invokeCallback(
    jsg::Lock& js, v8::Local<v8::Function> callback, jsg::Arguments<jsg::Value>& args) {
  v8::LocalVector<v8::Value> argv(js.v8Isolate);
  for (auto& arg: args) {
    argv.push_back(arg.getHandle(js));
  }
  auto v8Context = js.v8Context();
  return jsg::check(callback->Call(v8Context, v8Context->Global(), argv.size(), argv.data()));
}

v8::Local<v8::Value> runSpan(jsg::Lock& js,
    kj::String operationName,
    v8::Local<v8::Function> callback,
    jsg::Arguments<jsg::Value> args,
    const jsg::TypeHandler<jsg::Ref<user_tracing::Span>>& spanHandler,
    const jsg::TypeHandler<jsg::Promise<jsg::Value>>* valuePromiseHandler,
    SpanEndMode endMode) {
  auto createdSpan = createSpan(js, kj::mv(operationName));
  auto jsSpan = kj::mv(createdSpan.span);

  // Build argv for the callback: (span, ...args).
  v8::LocalVector<v8::Value> argv(js.v8Isolate);
  argv.push_back(spanHandler.wrap(js, jsSpan.addRef()));
  for (auto& arg: args) {
    argv.push_back(arg.getHandle(js));
  }

  auto executeCallback = [&]() -> v8::Local<v8::Value> {
    auto v8Context = js.v8Context();
    return js.tryCatch([&]() -> v8::Local<v8::Value> {
      auto result =
          jsg::check(callback->Call(v8Context, v8Context->Global(), argv.size(), argv.data()));

      if (endMode == SpanEndMode::MANUAL_END) {
        return result;
      }

      // If the callback returned a promise, defer end() until settlement.
      if (result->IsPromise()) {
        KJ_ASSERT(valuePromiseHandler != nullptr);
        auto promise = KJ_ASSERT_NONNULL(valuePromiseHandler->tryUnwrap(js, result))
                           .then(js,
                               [jsSpan = jsSpan.addRef()](
                                   jsg::Lock& js, jsg::Value value) mutable -> jsg::Value {
          jsSpan->end();
          return kj::mv(value);
        },
                               [jsSpan = jsSpan.addRef()](
                                   jsg::Lock& js, jsg::Value exception) mutable -> jsg::Value {
          jsSpan->end();
          js.throwException(kj::mv(exception));
        });
        // If the promise never settles, the span will still be submitted when the IoOwn is
        // destroyed (via ~SpanState calling end()), though this is a corner case and should
        // generally be avoided by users.
        return valuePromiseHandler->wrap(js, kj::mv(promise));
      } else {
        // Synchronous success: end immediately.
        jsSpan->end();
        return result;
      }
    }, [&](jsg::Value exception) -> v8::Local<v8::Value> {
      if (endMode == SpanEndMode::AUTO_END) {
        // Synchronous exception: end then rethrow.
        jsSpan->end();
      }
      js.throwException(kj::mv(exception));
    });
  };

  // childSpanForAsyncContext is only set when we have an IoContext and an observed child span.
  return runWithActiveSpan(
      js, jsSpan, kj::mv(createdSpan.childSpanForAsyncContext), spanHandler, executeCallback);
}

}  // namespace

v8::Local<v8::Value> Tracing::enterSpan(jsg::Lock& js,
    kj::String operationName,
    v8::Local<v8::Function> callback,
    jsg::Arguments<jsg::Value> args,
    const jsg::TypeHandler<jsg::Ref<user_tracing::Span>>& spanHandler,
    const jsg::TypeHandler<jsg::Promise<jsg::Value>>& valuePromiseHandler) {
  return runSpan(js, kj::mv(operationName), callback, kj::mv(args), spanHandler,
      &valuePromiseHandler, SpanEndMode::AUTO_END);
}

v8::Local<v8::Value> Tracing::startActiveSpan(jsg::Lock& js,
    kj::String operationName,
    v8::Local<v8::Function> callback,
    jsg::Arguments<jsg::Value> args,
    const jsg::TypeHandler<jsg::Ref<user_tracing::Span>>& spanHandler) {
  return runSpan(js, kj::mv(operationName), callback, kj::mv(args), spanHandler, nullptr,
      SpanEndMode::MANUAL_END);
}

jsg::Ref<user_tracing::Span> Tracing::startSpan(jsg::Lock& js, kj::String operationName) {
  return createSpan(js, kj::mv(operationName)).span;
}

namespace {
kj::String truncateUtf8(kj::String value, size_t limit) {
  if (value.size() <= limit) return kj::mv(value);
  while (limit > 0 && (static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80) --limit;
  return kj::str(value.first(limit));
}

kj::Maybe<tracing::SpanTimestamp> toNativeTimestamp(
    const jsg::Optional<TracingContext::Timestamp>& time) {
  KJ_IF_SOME(value, time) {
    if (value.seconds < 0 || value.nanoseconds >= 1000000000 || value.seconds > 9223372036 ||
        (value.seconds == 9223372036 && value.nanoseconds >= 854775808))
      return kj::none;
    return tracing::SpanTimestamp(value.seconds, value.nanoseconds);
  }
  return kj::none;
}
}  // namespace

void TracingContext::end(jsg::Ref<user_tracing::Span> span, jsg::Optional<Timestamp> time) {
  if (!IoContext::hasCurrent() || !IoContext::current().isOpentelemetryApiEnabled()) return;
  if (span->getIsTraced()) {
    KJ_IF_SOME(value, toNativeTimestamp(time)) {
      KJ_SWITCH_ONEOF(span->state) {
        KJ_CASE_ONEOF(state, kj::Own<user_tracing::SpanState>) {
          state->setEndTimestamp(value);
        }
        KJ_CASE_ONEOF(state, IoOwn<user_tracing::SpanState>) {
          state->setEndTimestamp(value);
        }
      }
    }
  }
  span->end();
}

void TracingContext::recordException(jsg::Lock& js,
    jsg::Ref<user_tracing::Span> span,
    jsg::Value exception,
    jsg::Optional<Timestamp> time,
    const jsg::TypeHandler<user_tracing::ExceptionData>& exceptionHandler) {
  if (!IoContext::hasCurrent() || !IoContext::current().isOpentelemetryApiEnabled()) return;
  span->recordExceptionWithTimestamp(
      js, kj::mv(exception), exceptionHandler, toNativeTimestamp(time));
}

void TracingContext::addEvent(jsg::Lock& js,
    jsg::Ref<user_tracing::Span> span,
    kj::String name,
    jsg::Dict<jsg::Optional<jsg::Value>> attributes,
    jsg::Optional<Timestamp> time,
    const jsg::TypeHandler<user_tracing::TagValue>& scalarHandler) {
  if (!IoContext::hasCurrent() || !IoContext::current().isOpentelemetryApiEnabled() ||
      !span->getIsTraced())
    return;
  kj::Vector<tracing::Attribute> values;
  for (auto& field: attributes.fields) {
    if (values.size() == 128) break;
    KJ_IF_SOME(value, field.value) {
      KJ_IF_SOME(converted,
          user_tracing::toAttributeValue(js, field.name, value.getHandle(js), scalarHandler)) {
        values.add(tracing::Attribute(kj::ConstString(kj::mv(field.name)), kj::mv(converted)));
      }
    }
  }
  auto timestamp = toNativeTimestamp(time);
  tracing::SpanEvent event(
      kj::ConstString(truncateUtf8(kj::mv(name), 64)), values.releaseAsArray(), timestamp);
  KJ_SWITCH_ONEOF(span->state) {
    KJ_CASE_ONEOF(state, kj::Own<user_tracing::SpanState>) {
      state->addEvent(kj::mv(event));
    }
    KJ_CASE_ONEOF(state, IoOwn<user_tracing::SpanState>) {
      state->addEvent(kj::mv(event));
    }
  }
}

void TracingContext::addLinks(jsg::Lock& js,
    jsg::Ref<user_tracing::Span> span,
    kj::Array<Link> input,
    const jsg::TypeHandler<user_tracing::TagValue>& scalarHandler) {
  if (!IoContext::hasCurrent() || !IoContext::current().isOpentelemetryApiEnabled() ||
      !span->getIsTraced())
    return;
  kj::Vector<tracing::SpanLink> output;
  for (auto& link: input) {
    if (output.size() == 128) break;
    auto parsed = tracing::SpanContext::tryFromTraceparent(
        kj::str("00-", link.context.traceId, "-", link.context.spanId, "-00"));
    if (parsed == kj::none) continue;
    auto& identity = KJ_ASSERT_NONNULL(parsed);
    kj::Vector<tracing::Attribute> values;
    for (auto& field: link.attributes.fields) {
      if (values.size() == 128) break;
      KJ_IF_SOME(value, field.value) {
        KJ_IF_SOME(converted,
            user_tracing::toAttributeValue(js, field.name, value.getHandle(js), scalarHandler)) {
          values.add(tracing::Attribute(kj::ConstString(kj::mv(field.name)), kj::mv(converted)));
        }
      }
    }
    kj::Maybe<kj::ConstString> serializedTraceState;
    KJ_IF_SOME(value, link.traceState) {
      KJ_IF_SOME(validated, tracing::SpanContext::parseTracestate(value)) {
        serializedTraceState = kj::ConstString(kj::mv(validated));
      }
    }
    output.add(tracing::SpanLink(tracing::SpanContext(identity.getTraceId(), identity.getSpanId(),
                                     tracing::TraceFlags(link.context.traceFlags)),
        values.releaseAsArray(),
        link.isRemote.orDefault(false) ? tracing::SpanLink::Origin::REMOTE
                                       : tracing::SpanLink::Origin::LOCAL,
        kj::mv(serializedTraceState)));
  }
  tracing::SpanLinks links(output.releaseAsArray());
  KJ_SWITCH_ONEOF(span->state) {
    KJ_CASE_ONEOF(state, kj::Own<user_tracing::SpanState>) {
      state->addLinks(kj::mv(links));
    }
    KJ_CASE_ONEOF(state, IoOwn<user_tracing::SpanState>) {
      state->addLinks(kj::mv(links));
    }
  }
}

jsg::Ref<user_tracing::Span> TracingContext::startSpan(
    jsg::Lock& js, kj::String operationName, SpanOptions options) {
  JSG_REQUIRE(!IoContext::hasCurrent() || IoContext::current().isOpentelemetryApiEnabled(), Error,
      "OpenTelemetry API is disabled");
  tracing::SpanMetadata metadata;
  metadata.startTime = toNativeTimestamp(options.startTime);
  JSG_REQUIRE(options.kind <= 4, TypeError, "Invalid span kind");
  metadata.kind = static_cast<rpc::SpanKind>(options.kind + 1);
  tracing::InstrumentationScope scope{
    kj::ConstString(truncateUtf8(kj::mv(options.scope.name), 256)), kj::none, kj::none};
  KJ_IF_SOME(value, options.scope.version) {
    scope.version = kj::ConstString(truncateUtf8(kj::mv(value), 256));
  }
  KJ_IF_SOME(value, options.scope.schemaUrl) {
    scope.schemaUrl = kj::ConstString(truncateUtf8(kj::mv(value), 2048));
  }
  metadata.scope = kj::mv(scope);
  return createSpan(js, kj::mv(operationName), kj::mv(metadata),
      options.root.orDefault(false) ? DetachedRoot::YES : DetachedRoot::NO)
      .span;
}

v8::Local<v8::Value> Tracing::withActiveSpan(jsg::Lock& js,
    jsg::Ref<user_tracing::Span> span,
    v8::Local<v8::Function> callback,
    jsg::Arguments<jsg::Value> args,
    const jsg::TypeHandler<jsg::Ref<user_tracing::Span>>& spanHandler) {
  // A no-op span leaves the current parent in place. In particular, it must not reset an
  // already-suppressed context to the invocation's recorded parent.
  kj::Maybe<SpanParent> parent;
  if (IoContext::hasCurrent()) {
    auto spanParent = span->makeSpanParent();
    if (spanParent.isObserved()) parent = kj::mv(spanParent);
  }

  return runWithActiveSpan(js, span, kj::mv(parent), spanHandler,
      [&]() -> v8::Local<v8::Value> { return invokeCallback(js, callback, args); });
}

jsg::Optional<jsg::Ref<user_tracing::Span>> Tracing::getActiveSpan(
    jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<user_tracing::Span>>& spanHandler) {
  // case: a user has an active span
  //
  // tracing.startActiveSpan('operation', async (span) => {
  //   tracing.getActiveSpan() === span;
  // });
  KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(js)) {
    auto key = jsg::IsolateBase::from(js.v8Isolate).getActiveSpanAsyncContextKey();
    KJ_IF_SOME(value, frame.get(*key)) {
      if (value.getHandle(js)->IsUndefined()) return kj::none;
      KJ_IF_SOME(span, spanHandler.tryUnwrap(js, value.getHandle(js))) {
        return kj::mv(span);
      }
    }
  }

  // case: outside an invocation
  if (!IoContext::hasCurrent()) {
    return kj::none;
  }

  // case: inside an invocation with no explicit child span set
  // this is cached so that repeated calls return the same reference
  auto& ioContext = IoContext::current();
  KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(js)) {
    auto key = ioContext.getCurrentLock().getUserTraceAsyncContextKey();
    KJ_IF_SOME(value, frame.get(*key)) {
      auto holder = value.getHandle(js).As<v8::Object>();
      auto& scopeState = jsg::unwrapOpaqueRef<IoContext::UserTraceScopeState>(js.v8Isolate, holder);
      auto cacheKey = v8::Private::ForApi(js.v8Isolate, js.strIntern("workerd.activeSpan"_kjc));
      if (jsg::check(holder->HasPrivate(js.v8Context(), cacheKey))) {
        auto cached = jsg::check(holder->GetPrivate(js.v8Context(), cacheKey));
        KJ_IF_SOME(span, spanHandler.tryUnwrap(js, cached)) {
          return kj::mv(span);
        }
      }

      kj::Own<user_tracing::SpanState> state;
      KJ_SWITCH_ONEOF(scopeState) {
        KJ_CASE_ONEOF(id, IoContext::Id) {
          if (!id.isCurrent()) IoContext::throwNotCurrentJsError(typeid(UserTraceAsyncContext));
          state = kj::refcounted<user_tracing::NoopSpanState>();
        }
        KJ_CASE_ONEOF(asyncContext, IoOwn<UserTraceAsyncContext>) {
          kj::Maybe<kj::Own<BaseTracer::WeakRef>> tracer;
          KJ_IF_SOME(value, asyncContext->getTracer()) {
            tracer = value.addRef();
          }
          state = kj::refcounted<user_tracing::InvocationSpanState>(asyncContext->getSpan(),
              kj::mv(tracer),
              ioContext.isOpentelemetryApiEnabled() ? asyncContext->getSpanInvocationContext()
                                                    : asyncContext->getInvocationSpanContext().map(
                                                          [](auto& ctx) { return ctx.clone(); }),
              ioContext.getDefaultJsTraceFlags());
        }
      }
      auto span = js.alloc<user_tracing::Span>(ioContext.addObject(kj::mv(state)));
      auto wrapped = spanHandler.wrap(js, span.addRef());
      jsg::check(holder->SetPrivate(js.v8Context(), cacheKey, wrapped));
      return span;
    }
  }

  return kj::none;
}

}  // namespace workerd::api
