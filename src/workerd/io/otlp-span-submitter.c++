// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "otlp-span-submitter.h"

#include <workerd/util/thread-scopes.h>
#include <workerd/util/uuid.h>

#include <kj-rs/kj-rs.h>

namespace workerd {

namespace {

int64_t toUnixNanos(kj::Date date) {
  return (date - kj::UNIX_EPOCH) / kj::NANOSECONDS;
}

// KJ strings need not be UTF-8, so text crosses to Rust as bytes.
::rust::Slice<const kj::byte> toRust(kj::StringPtr text) {
  return text.asBytes().as<kj_rs::Rust>();
}

rust::otlp::StatusCode toRust(tracing::SpanStatusCode code) {
  switch (code) {
    case tracing::SpanStatusCode::UNSET:
      return rust::otlp::StatusCode::Unset;
    case tracing::SpanStatusCode::OK:
      return rust::otlp::StatusCode::Ok;
    case tracing::SpanStatusCode::ERROR:
      return rust::otlp::StatusCode::Error;
  }
  KJ_UNREACHABLE;
}

void addTag(rust::otlp::Attributes& attributes, const Span::Tag& tag) {
  auto key = toRust(tag.key);
  KJ_SWITCH_ONEOF(tag.value) {
    KJ_CASE_ONEOF(b, bool) {
      rust::otlp::add_bool(attributes, key, b);
    }
    KJ_CASE_ONEOF(i, int64_t) {
      rust::otlp::add_int(attributes, key, i);
    }
    KJ_CASE_ONEOF(d, double) {
      rust::otlp::add_double(attributes, key, d);
    }
    KJ_CASE_ONEOF(s, kj::ConstString) {
      rust::otlp::add_string(attributes, key, toRust(s));
    }
  }
}

// `Tags` is a Span::TagMap or an array of Span::Tag.
template <typename Tags>
::rust::Box<rust::otlp::Attributes> toAttributes(const Tags& tags) {
  auto attributes = rust::otlp::new_attributes();
  for (auto& tag: tags) addTag(*attributes, tag);
  return attributes;
}

::rust::Box<rust::otlp::SpanBuffer> newSpanBuffer(const tracing::SpanContext& context,
    tracing::SpanId rootSpanId,
    const OtlpSpanSubmitter::Options& options) {
  auto resource = rust::otlp::new_attributes();
  rust::otlp::add_string(*resource, toRust("service.name"_kj), toRust(options.serviceName));
  for (auto& tag: options.resource) addTag(*resource, tag);

  auto traceId = context.getTraceId().toProtobuf();
  uint32_t traceFlags = 0;
  KJ_IF_SOME(flags, context.getTraceFlags()) {
    traceFlags = uint8_t(flags);
  }
  return rust::otlp::new_span_buffer(traceId.as<kj_rs::Rust>(),
      {
        .trace_flags = traceFlags,
        .root_span_id = rootSpanId.getId(),
        .redact_query_string = bool(options.redactQueryString),
        // As for Jaeger spans, so tests can compare whole spans.
        .omit_timestamps = isPredictableModeForTest(),
      },
      kj::mv(resource), toAttributes(options.defaultTags));
}

Span::TagValue text(kj::StringPtr value) {
  return kj::ConstString(kj::str(value));
}

}  // namespace

kj::Array<Span::Tag> makeOtlpResource(const OtlpServiceInfo& service) {
  using Tag = Span::Tag;

  kj::Vector<Tag> tags;
  tags.add(Tag{"telemetry.sdk.name"_kjc, text("workers-runtime")});
  tags.add(Tag{"telemetry.sdk.language"_kjc, text("js")});
  KJ_IF_SOME(name, service.scriptName) {
    tags.add(Tag{"faas.name"_kjc, text(name)});
    tags.add(Tag{"cloudflare.script_name"_kjc, text(name)});
  }
  KJ_IF_SOME(version, service.scriptVersion) {
    auto id = version.getId();
    KJ_IF_SOME(uuid, UUID::fromUpperLower(id.getUpper(), id.getLower())) {
      tags.add(Tag{"faas.version"_kjc, text(uuid.toString())});
      tags.add(Tag{"cloudflare.script_version.id"_kjc, text(uuid.toString())});
    }
    if (version.hasTag()) {
      tags.add(Tag{"cloudflare.script_version.tag"_kjc, text(version.getTag())});
    }
    if (version.hasMessage()) {
      tags.add(Tag{"cloudflare.script_version.message"_kjc, text(version.getMessage())});
    }
  }
  KJ_IF_SOME(ns, service.dispatchNamespace) {
    tags.add(Tag{"cloudflare.dispatch_namespace"_kjc, text(ns)});
  }
  KJ_IF_SOME(entrypoint, service.entrypoint) {
    tags.add(Tag{"cloudflare.entrypoint"_kjc, text(entrypoint)});
  }
  KJ_IF_SOME(colo, service.colo) {
    tags.add(Tag{"cloudflare.colo"_kjc, text(colo)});
  }
  KJ_IF_SOME(region, service.region) {
    tags.add(Tag{"faas.invoked_region"_kjc, text(region)});
  }
  KJ_IF_SOME(preview, service.preview) {
    tags.add(Tag{"cloudflare.preview.id"_kjc, text(preview.id)});
    tags.add(Tag{"cloudflare.preview.slug"_kjc, text(preview.slug)});
    tags.add(Tag{"cloudflare.preview.name"_kjc, text(preview.name)});
  }
  if (service.scriptTags.size() > 0) {
    // Span::TagValue has no array form, so the tags travel as one comma-separated string.
    tags.add(Tag{"cloudflare.script_tags"_kjc, text(kj::strArray(service.scriptTags, ","))});
  }
  constexpr auto ENVIRONMENT_TAG = "cf:environment="_kj;
  for (auto& tag: service.scriptTags) {
    if (tag.startsWith(ENVIRONMENT_TAG)) {
      tags.add(Tag{"deployment.environment.name"_kjc, text(tag.slice(ENVIRONMENT_TAG.size()))});
    }
  }
  return tags.releaseAsArray();
}

kj::Array<Span::Tag> makeOtlpIdentityTags(const OtlpServiceInfo& service) {
  kj::Vector<Span::Tag> tags(2);
  KJ_IF_SOME(entrypoint, service.entrypoint) {
    tags.add(Span::Tag{"cloudflare.entrypoint"_kjc, text(entrypoint)});
  }
  KJ_IF_SOME(id, service.durableObjectId) {
    tags.add(Span::Tag{"cloudflare.durable_object.id"_kjc, text(id)});
  }
  return tags.releaseAsArray();
}

SpanBuilder startExportedUserSpan(kj::EntropySource& entropySource,
    kj::Own<OtlpSpanExporter> exporter,
    const tracing::SpanContext& context,
    const OtlpSpanSubmitter::Options& options,
    kj::ConstString operationName,
    tracing::SpanId spanId) {
  auto submitter =
      kj::refcounted<OtlpSpanSubmitter>(entropySource, kj::mv(exporter), context, options, spanId);
  // The submitter records this span itself, so its observer carries its span id and its parent's.
  auto rootSpanId = submitter->getRootSpanId();
  return SpanBuilder(
      kj::rc<UserSpanObserver>(kj::mv(submitter), context.getTraceId(), context.getTraceFlags(),
          rootSpanId, context.getSpanId().orDefault(tracing::SpanId::nullId)),
      kj::mv(operationName));
}

kj::Promise<void> postOtlpTraces(kj::HttpClient& client,
    kj::StringPtr url,
    kj::HttpHeaders headers,
    kj::Array<const kj::byte> body) {
  headers.setPtr(kj::HttpHeaderId::CONTENT_TYPE, "application/x-protobuf");
  auto request = client.request(kj::HttpMethod::POST, url, headers, body.size());
  co_await request.body->write(body);
  request.body = nullptr;
  auto response = co_await request.response;
  KJ_REQUIRE(response.statusCode / 100 == 2, "OTLP collector rejected spans", response.statusCode,
      response.statusText);
  // Drain the body so the connection can be reused.
  co_await response.body->readAllBytes();
}

OtlpSpanSubmitter::OtlpSpanSubmitter(kj::EntropySource& entropySource,
    kj::Own<OtlpSpanExporter> exporter,
    const tracing::SpanContext& context,
    Options options,
    tracing::SpanId rootSpan)
    : entropySource(entropySource),
      predictableSpanId((uint64_t(options.predictableSpanSpace) << 56) + 1),
      exporter(kj::mv(exporter)),
      rootSpanId(rootSpan == tracing::SpanId::nullId ? makeSpanId() : rootSpan),
      buffer(newSpanBuffer(context, rootSpanId, options)) {}

OtlpSpanSubmitter::~OtlpSpanSubmitter() noexcept(false) {
  exportBatch(rust::otlp::finish(*buffer, toUnixNanos(kj::systemPreciseCalendarClock().now())));
}

bool OtlpSpanSubmitter::submitSpanOpen(tracing::SpanId spanId,
    tracing::SpanId parentSpanId,
    kj::ConstString operationName,
    kj::Date startTime) {
  // The root is named after the handler, not after a runtime span (see describeInvocationSpan()).
  if (spanId != rootSpanId && !exporter->exportsRuntimeSpan(operationName)) {
    return false;
  }
  return open(spanId, parentSpanId, operationName, startTime);
}

bool OtlpSpanSubmitter::submitUserSpanOpen(tracing::SpanId spanId,
    tracing::SpanId parentSpanId,
    kj::ConstString operationName,
    kj::Date startTime) {
  if (!exporter->exportsUserSpan(operationName)) {
    return false;
  }
  return open(spanId, parentSpanId, operationName, startTime);
}

bool OtlpSpanSubmitter::open(tracing::SpanId spanId,
    tracing::SpanId parentSpanId,
    kj::StringPtr operationName,
    kj::Date startTime) {
  if (rust::otlp::open_span(*buffer, spanId.getId(), parentSpanId.getId(), toRust(operationName),
          toUnixNanos(startTime))) {
    return true;
  }
  exporter->spansDropped(1);
  return false;
}

void OtlpSpanSubmitter::submitSpanClose(
    tracing::SpanId spanId, kj::Date startTime, kj::Date endTime, Span::TagMap&& tags) {
  exportBatch(
      rust::otlp::close_span(*buffer, spanId.getId(), toUnixNanos(endTime), toAttributes(tags)));
}

void OtlpSpanSubmitter::submitSpanUpdate(tracing::SpanId spanId, tracing::SpanUpdate&& update) {
  KJ_SWITCH_ONEOF(update.info) {
    KJ_CASE_ONEOF(operationName, kj::ConstString) {
      rust::otlp::set_span_name(*buffer, spanId.getId(), toRust(operationName));
    }
    KJ_CASE_ONEOF(status, tracing::SpanStatus) {
      kj::StringPtr message;
      KJ_IF_SOME(m, status.getMessage()) message = m;
      rust::otlp::set_span_status(
          *buffer, spanId.getId(), toRust(status.getCode()), toRust(message));
    }
  }
}

void OtlpSpanSubmitter::submitSpanException(tracing::SpanId spanId,
    kj::Date timestamp,
    kj::Maybe<tracing::Exception::Code> code,
    kj::String name,
    kj::String message,
    kj::Maybe<kj::String> stack) {
  rust::otlp::add_span_exception(*buffer, spanId.getId(), toUnixNanos(timestamp), toRust(name),
      toRust(message), stack.map([](kj::String& s) { return toRust(s); }));
}

tracing::SpanId OtlpSpanSubmitter::makeSpanId() {
  if (isPredictableModeForTest()) {
    return tracing::SpanId(predictableSpanId++);
  }
  return tracing::SpanId::fromEntropy(entropySource);
}

void OtlpSpanSubmitter::exportBatch(rust::otlp::Batch batch) {
  if (batch.span_count == 0) return;
  auto request = kj::from<kj_rs::Rust>(batch.request).attach(kj::mv(batch.request));
  exporter->exportSpans(kj::mv(request), batch.span_count);
}

}  // namespace workerd
