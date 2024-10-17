// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "trace-common.h"

#include "trace-legacy.h"

#include <workerd/jsg/jsg.h>

#include <capnp/message.h>
#include <capnp/schema.h>
#include <kj/compat/http.h>

namespace workerd::trace {

// ======================================================================================
// Tag
namespace {
Tags getTagsFromReader(const capnp::List<rpc::Trace::Tag>::Reader& tags) {
  kj::Vector<Tag> results;
  results.reserve(tags.size());
  for (auto tag: tags) {
    results.add(Tag(tag));
  }
  return results.releaseAsArray();
}

Tag::TagValue getTagValue(const rpc::Trace::Tag::Reader& reader) {
  auto value = reader.getValue();
  switch (value.which()) {
    case rpc::Trace::Tag::Value::Which::BOOL: {
      return value.getBool();
    }
    case rpc::Trace::Tag::Value::Which::INT64: {
      return value.getInt64();
    }
    case rpc::Trace::Tag::Value::Which::UINT64: {
      return value.getUint64();
    }
    case rpc::Trace::Tag::Value::Which::FLOAT64: {
      return value.getFloat64();
    }
    case rpc::Trace::Tag::Value::Which::TEXT: {
      return kj::str(value.getText());
    }
    case rpc::Trace::Tag::Value::Which::DATA: {
      return kj::heapArray<kj::byte>(value.getData());
    }
  }
  KJ_UNREACHABLE;
}

Tag::TagKey getTagKey(const rpc::Trace::Tag::Reader& reader) {
  auto key = reader.getKey();
  switch (key.which()) {
    case rpc::Trace::Tag::Key::Which::TEXT: {
      return kj::str(key.getText());
    }
    case rpc::Trace::Tag::Key::Which::ID: {
      return key.getId();
    }
  }
  KJ_UNREACHABLE;
}
Tags maybeGetTags(const auto& reader) {
  if (!reader.hasTags()) return nullptr;
  return getTagsFromReader(reader.getTags());
}
}  // namespace

Tag::Tag(TagKey key, TagValue value): key(kj::mv(key)), value(kj::mv(value)) {}

Tag::Tag(rpc::Trace::Tag::Reader reader): key(getTagKey(reader)), value(getTagValue(reader)) {}

Tag Tag::clone() const {
  TagKey newKey = ([&]() -> TagKey {
    KJ_SWITCH_ONEOF(key) {
      KJ_CASE_ONEOF(id, uint32_t) {
        return id;
      }
      KJ_CASE_ONEOF(name, kj::String) {
        return kj::str(name);
      }
    }
    KJ_UNREACHABLE;
  })();
  TagValue newValue = ([&]() -> TagValue {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(b, bool) {
        return b;
      }
      KJ_CASE_ONEOF(i, int64_t) {
        return i;
      }
      KJ_CASE_ONEOF(u, uint64_t) {
        return u;
      }
      KJ_CASE_ONEOF(d, double) {
        return d;
      }
      KJ_CASE_ONEOF(s, kj::String) {
        return kj::str(s);
      }
      KJ_CASE_ONEOF(a, kj::Array<kj::byte>) {
        return kj::heapArray<kj::byte>(a);
      }
    }
    KJ_UNREACHABLE;
  })();
  return Tag(kj::mv(newKey), kj::mv(newValue));
}

void Tag::copyTo(rpc::Trace::Tag::Builder builder) const {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(id, uint32_t) {
      builder.getKey().setId(id);
    }
    KJ_CASE_ONEOF(name, kj::String) {
      builder.getKey().setText(name);
    }
  }
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(b, bool) {
      builder.getValue().setBool(b);
    }
    KJ_CASE_ONEOF(i, int64_t) {
      builder.getValue().setInt64(i);
    }
    KJ_CASE_ONEOF(u, uint64_t) {
      builder.getValue().setUint64(u);
    }
    KJ_CASE_ONEOF(d, double) {
      builder.getValue().setFloat64(d);
    }
    KJ_CASE_ONEOF(s, kj::String) {
      builder.getValue().setText(s);
    }
    KJ_CASE_ONEOF(a, kj::Array<kj::byte>) {
      builder.getValue().setData(a);
    }
  }
}

bool Tag::keyMatches(kj::OneOf<kj::StringPtr, uint32_t> check) {
  KJ_SWITCH_ONEOF(check) {
    KJ_CASE_ONEOF(c, kj::StringPtr) {
      KJ_IF_SOME(k, key.tryGet<kj::String>()) {
        return k == c;
      }
    }
    KJ_CASE_ONEOF(u, uint32_t) {
      KJ_IF_SOME(k, key.tryGet<uint32_t>()) {
        return k == u;
      }
    }
  }
  return false;
}

jsg::JsObject Tag::toObject(jsg::Lock& js,
    kj::ArrayPtr<const Tag> tags,
    NameProvider nameProvider,
    ToObjectOptions options) {
  auto const getTags = [&] {
    auto tagObj = js.obj();
    for (auto& tag: tags) {
      auto maybeName = ([&]() -> kj::Maybe<kj::StringPtr> {
        KJ_SWITCH_ONEOF(tag.key) {
          KJ_CASE_ONEOF(name, kj::String) {
            return name.asPtr();
          }
          KJ_CASE_ONEOF(id, uint32_t) {
            return nameProvider(id, NameProviderContext::TAG);
          }
        }
        KJ_UNREACHABLE;
      })();

      KJ_IF_SOME(name, maybeName) {
        auto value = ([&]() -> jsg::JsValue {
          KJ_SWITCH_ONEOF(tag.value) {
            KJ_CASE_ONEOF(b, bool) {
              return js.boolean(b);
            }
            KJ_CASE_ONEOF(i, int64_t) {
              return js.bigInt(i);
            }
            KJ_CASE_ONEOF(u, uint64_t) {
              return js.bigInt(u);
            }
            KJ_CASE_ONEOF(d, double) {
              return js.num(d);
            }
            KJ_CASE_ONEOF(s, kj::String) {
              return js.str(s);
            }
            KJ_CASE_ONEOF(a, kj::Array<kj::byte>) {
              return jsg::JsValue(js.bytes(kj::heapArray<kj::byte>(a)).getHandle(js));
            }
          }
          KJ_UNREACHABLE;
        })();

        if (tagObj.has(js, name)) {
          auto existing = tagObj.get(js, name);
          KJ_IF_SOME(arr, existing.tryCast<jsg::JsArray>()) {
            arr.add(js, value);
          } else {
            tagObj.set(js, name, js.arr(existing, value));
          }
        } else {
          tagObj.set(js, name, value);
        }
      }
    };
    return tagObj;
  };

  auto tagsObj = getTags();
  if (options == ToObjectOptions::WRAPPED) {
    auto obj = js.obj();
    obj.set(js, "type", js.str("custom"_kj));
    obj.set(js, "tags", tagsObj);
    return obj;
  }
  return tagsObj;
}

// ======================================================================================
// Onset

namespace {
kj::Maybe<kj::String> maybeGetScriptName(const rpc::Trace::Onset::Reader& reader) {
  if (!reader.hasScriptName()) return kj::none;
  return kj::str(reader.getScriptName());
}
kj::Maybe<kj::Own<ScriptVersion::Reader>> maybeGetScriptVersion(
    const rpc::Trace::Onset::Reader& reader) {
  if (!reader.hasScriptVersion()) return kj::none;
  return capnp::clone(reader.getScriptVersion());
}
kj::Maybe<kj::String> maybeGetDispatchNamespace(const rpc::Trace::Onset::Reader& reader) {
  if (!reader.hasDispatchNamespace()) return kj::none;
  return kj::str(reader.getDispatchNamespace());
}
kj::Maybe<kj::String> maybeGetScriptId(const rpc::Trace::Onset::Reader& reader) {
  if (!reader.hasScriptId()) return kj::none;
  return kj::str(reader.getScriptId());
}
kj::Array<kj::String> maybeGetScriptTags(const rpc::Trace::Onset::Reader& reader) {
  kj::Vector<kj::String> results(reader.getScriptTags().size());
  for (auto tag: reader.getScriptTags()) {
    results.add(kj::str(tag));
  }
  return results.releaseAsArray();
}
kj::Maybe<kj::String> maybeGetEntrypoint(const rpc::Trace::Onset::Reader& reader) {
  if (!reader.hasEntrypoint()) return kj::none;
  return kj::str(reader.getEntrypoint());
}
kj::Maybe<EventInfo> maybeGetEventInfo(const rpc::Trace::Onset::Reader& reader) {
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::Onset::Info::Which::NONE:
      return kj::none;
    case rpc::Trace::Onset::Info::Which::FETCH: {
      return kj::Maybe(FetchEventInfo(info.getFetch()));
    }
    case rpc::Trace::Onset::Info::Which::JS_RPC: {
      return kj::Maybe(JsRpcEventInfo(info.getJsRpc()));
    }
    case rpc::Trace::Onset::Info::Which::SCHEDULED: {
      return kj::Maybe(ScheduledEventInfo(info.getScheduled()));
    }
    case rpc::Trace::Onset::Info::Which::ALARM: {
      return kj::Maybe(AlarmEventInfo(info.getAlarm()));
    }
    case rpc::Trace::Onset::Info::Which::QUEUE: {
      return kj::Maybe(QueueEventInfo(info.getQueue()));
    }
    case rpc::Trace::Onset::Info::Which::EMAIL: {
      return kj::Maybe(EmailEventInfo(info.getEmail()));
    }
    case rpc::Trace::Onset::Info::Which::TRACE: {
      return kj::Maybe(TraceEventInfo(info.getTrace()));
    }
    case rpc::Trace::Onset::Info::Which::HIBERNATABLE_WEB_SOCKET: {
      return kj::Maybe(HibernatableWebSocketEventInfo(info.getHibernatableWebSocket()));
    }
    case rpc::Trace::Onset::Info::Which::CUSTOM: {
      auto custom = info.getCustom();
      kj::Vector<Tag> results(custom.size());
      for (auto c: custom) {
        results.add(Tag(c));
      }
      return kj::Maybe(results.releaseAsArray());
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<EventInfo> cloneEventInfo(const kj::Maybe<EventInfo>& other) {
  KJ_IF_SOME(e, other) {
    KJ_SWITCH_ONEOF(e) {
      KJ_CASE_ONEOF(fetch, FetchEventInfo) {
        return kj::Maybe(fetch.clone());
      }
      KJ_CASE_ONEOF(jsRpc, JsRpcEventInfo) {
        return kj::Maybe(jsRpc.clone());
      }
      KJ_CASE_ONEOF(scheduled, ScheduledEventInfo) {
        return kj::Maybe(scheduled.clone());
      }
      KJ_CASE_ONEOF(alarm, AlarmEventInfo) {
        return kj::Maybe(alarm.clone());
      }
      KJ_CASE_ONEOF(queue, QueueEventInfo) {
        return kj::Maybe(queue.clone());
      }
      KJ_CASE_ONEOF(email, EmailEventInfo) {
        return kj::Maybe(email.clone());
      }
      KJ_CASE_ONEOF(trace, TraceEventInfo) {
        return kj::Maybe(trace.clone());
      }
      KJ_CASE_ONEOF(hibWs, HibernatableWebSocketEventInfo) {
        return kj::Maybe(hibWs.clone());
      }
      KJ_CASE_ONEOF(custom, CustomEventInfo) {
        // TODO(streaming-trace): Implement correctly
        return kj::Maybe(CustomEventInfo());
      }
      KJ_CASE_ONEOF(custom, Tags) {
        kj::Vector<Tag> newTags(custom.size());
        for (auto& tag: custom) {
          newTags.add(tag.clone());
        }
        return kj::Maybe(newTags.releaseAsArray());
      }
    }
  }
  return kj::none;
}
}  // namespace

Onset::Onset(kj::Maybe<kj::String> scriptName,
    kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
    kj::Maybe<kj::String> dispatchNamespace,
    kj::Maybe<kj::String> scriptId,
    kj::Array<kj::String> scriptTags,
    kj::Maybe<kj::String> entrypoint,
    ExecutionModel executionModel,
    Tags tags)
    : scriptName(kj::mv(scriptName)),
      scriptVersion(kj::mv(scriptVersion)),
      dispatchNamespace(kj::mv(dispatchNamespace)),
      scriptId(kj::mv(scriptId)),
      scriptTags(kj::mv(scriptTags)),
      entrypoint(kj::mv(entrypoint)),
      executionModel(executionModel),
      tags(kj::mv(tags)) {}

Onset::Onset(rpc::Trace::Onset::Reader reader)
    : scriptName(maybeGetScriptName(reader)),
      scriptVersion(maybeGetScriptVersion(reader)),
      dispatchNamespace(maybeGetDispatchNamespace(reader)),
      scriptId(maybeGetScriptId(reader)),
      scriptTags(maybeGetScriptTags(reader)),
      entrypoint(maybeGetEntrypoint(reader)),
      executionModel(reader.getExecutionModel()),
      info(maybeGetEventInfo(reader)),
      tags(maybeGetTags(reader)) {}

void Onset::copyTo(rpc::Trace::Onset::Builder builder) const {
  KJ_IF_SOME(name, scriptName) {
    builder.setScriptName(name);
  }
  KJ_IF_SOME(version, scriptVersion) {
    builder.setScriptVersion(*version);
  }
  KJ_IF_SOME(ns, dispatchNamespace) {
    builder.setDispatchNamespace(ns);
  }
  KJ_IF_SOME(id, scriptId) {
    builder.setScriptId(id);
  }
  if (scriptTags.size() > 0) {
    auto list = builder.initScriptTags(scriptTags.size());
    for (auto i: kj::indices(scriptTags)) {
      list.set(i, scriptTags[i]);
    }
  }
  KJ_IF_SOME(e, entrypoint) {
    builder.setEntrypoint(e);
  }
  builder.setExecutionModel(executionModel);

  KJ_IF_SOME(i, info) {
    auto infoBuilder = builder.initInfo();
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, FetchEventInfo) {
        fetch.copyTo(infoBuilder.initFetch());
      }
      KJ_CASE_ONEOF(jsRpc, JsRpcEventInfo) {
        jsRpc.copyTo(infoBuilder.initJsRpc());
      }
      KJ_CASE_ONEOF(scheduled, ScheduledEventInfo) {
        scheduled.copyTo(infoBuilder.initScheduled());
      }
      KJ_CASE_ONEOF(alarm, AlarmEventInfo) {
        alarm.copyTo(infoBuilder.initAlarm());
      }
      KJ_CASE_ONEOF(queue, QueueEventInfo) {
        queue.copyTo(infoBuilder.initQueue());
      }
      KJ_CASE_ONEOF(email, EmailEventInfo) {
        email.copyTo(infoBuilder.initEmail());
      }
      KJ_CASE_ONEOF(trace, TraceEventInfo) {
        trace.copyTo(infoBuilder.initTrace());
      }
      KJ_CASE_ONEOF(hibWs, HibernatableWebSocketEventInfo) {
        hibWs.copyTo(infoBuilder.initHibernatableWebSocket());
      }
      KJ_CASE_ONEOF(custom, Tags) {
        auto list = infoBuilder.initCustom(custom.size());
        for (auto i: kj::indices(custom)) {
          custom[i].copyTo(list[i]);
        }
      }
      KJ_CASE_ONEOF(custom, CustomEventInfo) {
        // We don't use the empty CustomEventInfo with the streaming trace.
        // So this should never be called.
        KJ_UNREACHABLE;
      }
    }
  }

  if (tags.size() > 0) {
    auto list = builder.initTags(tags.size());
    for (auto i: kj::indices(tags)) {
      tags[i].copyTo(list[i]);
    }
  }
}

Onset Onset::clone() const {
  Onset onset(scriptName.map([](const kj::String& s) { return kj::str(s); }),
      scriptVersion.map([](const kj::Own<ScriptVersion::Reader>& s) { return capnp::clone(*s); }),
      dispatchNamespace.map([](const kj::String& s) { return kj::str(s); }),
      scriptId.map([](const kj::String& s) { return kj::str(s); }),
      KJ_MAP(tag, scriptTags) { return kj::str(tag); },
      entrypoint.map([](const kj::String& s) { return kj::str(s); }), executionModel,
      KJ_MAP(tag, tags) { return tag.clone(); });
  onset.info = cloneEventInfo(info);
  return kj::mv(onset);
}

jsg::JsObject Onset::toObject(jsg::Lock& js, NameProvider nameProvider) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("onset"_kj));

  KJ_IF_SOME(name, scriptName) {
    obj.set(js, "scriptName", js.str(name));
  }
  KJ_IF_SOME(version, scriptVersion) {
    obj.set(js, "scriptVersion", js.str(kj::str(version)));
  }
  KJ_IF_SOME(ns, dispatchNamespace) {
    obj.set(js, "dispatchNamespace", js.str(ns));
  }
  KJ_IF_SOME(id, scriptId) {
    obj.set(js, "scriptId", js.str(id));
  }
  kj::Vector<jsg::JsValue> vec(scriptTags.size());
  for (auto& tag: scriptTags) {
    vec.add(js.str(tag));
  }
  obj.set(js, "scriptTags", js.arr(vec.releaseAsArray()));
  KJ_IF_SOME(e, entrypoint) {
    obj.set(js, "entrypoint", js.str(e));
  }

  switch (executionModel) {
    case ExecutionModel::DURABLE_OBJECT: {
      obj.set(js, "executionModel", js.str("durable-object"_kj));
      break;
    }
    case ExecutionModel::STATELESS: {
      obj.set(js, "executionModel", js.str("stateless"_kj));
      break;
    }
    case ExecutionModel::WORKFLOW: {
      obj.set(js, "executionModel", js.str("workflow"_kj));
      break;
    }
  }

  KJ_IF_SOME(i, info) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, FetchEventInfo) {
        obj.set(js, "info", fetch.toObject(js));
      }
      KJ_CASE_ONEOF(jsRpc, JsRpcEventInfo) {
        obj.set(js, "info", jsRpc.toObject(js));
      }
      KJ_CASE_ONEOF(scheduled, ScheduledEventInfo) {
        obj.set(js, "info", scheduled.toObject(js));
      }
      KJ_CASE_ONEOF(alarm, AlarmEventInfo) {
        obj.set(js, "info", alarm.toObject(js));
      }
      KJ_CASE_ONEOF(queue, QueueEventInfo) {
        obj.set(js, "info", queue.toObject(js));
      }
      KJ_CASE_ONEOF(email, EmailEventInfo) {
        obj.set(js, "info", email.toObject(js));
      }
      KJ_CASE_ONEOF(trace, TraceEventInfo) {
        obj.set(js, "info", trace.toObject(js));
      }
      KJ_CASE_ONEOF(hibWs, HibernatableWebSocketEventInfo) {
        obj.set(js, "info", hibWs.toObject(js));
      }
      KJ_CASE_ONEOF(custom, CustomEventInfo) {
        auto obj = js.obj();
        obj.set(js, "type", js.str("custom"_kj));
        obj.set(js, "info", obj);
      }
      KJ_CASE_ONEOF(custom, Tags) {
        obj.set(js, "info", Tag::toObject(js, custom, nameProvider));
      }
    }
  }

  if (tags.size() > 0) {
    obj.set(js, "tags", Tag::toObject(js, tags, nameProvider, Tag::ToObjectOptions::UNWRAPPED));
  }

  return obj;
}

// ======================================================================================
// Outcome

namespace {
kj::Maybe<Outcome::Info> maybeGetInfo(const rpc::Trace::Outcome::Reader& reader) {
  //  if (!reader.hasInfo()) return kj::none;
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::Outcome::Info::Which::NONE: {
      return kj::none;
    }
    case rpc::Trace::Outcome::Info::Which::FETCH: {
      return kj::Maybe(FetchResponseInfo(info.getFetch()));
    }
    case rpc::Trace::Outcome::Info::Which::CUSTOM: {
      auto custom = info.getCustom();
      kj::Vector<Tag> tags(custom.size());
      for (size_t n = 0; n < custom.size(); n++) {
        tags.add(Tag(custom[n]));
      }
      return kj::Maybe(tags.releaseAsArray());
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<Outcome::Info> cloneInfo(const kj::Maybe<Outcome::Info>& other) {
  KJ_IF_SOME(o, other) {
    KJ_SWITCH_ONEOF(o) {
      KJ_CASE_ONEOF(fetch, FetchResponseInfo) {
        return kj::Maybe(fetch.clone());
      }
      KJ_CASE_ONEOF(tags, Tags) {
        kj::Vector<Tag> newTags(tags.size());
        for (auto& tag: tags) {
          newTags.add(tag.clone());
        }
        return kj::Maybe(newTags.releaseAsArray());
      }
    }
  }
  return kj::none;
}
}  // namespace

Outcome::Outcome(EventOutcome outcome, kj::Maybe<Info> info)
    : outcome(outcome),
      info(kj::mv(info)) {}

Outcome::Outcome(rpc::Trace::Outcome::Reader reader)
    : outcome(reader.getOutcome()),
      info(maybeGetInfo(reader)) {}

void Outcome::copyTo(rpc::Trace::Outcome::Builder builder) const {
  builder.setOutcome(outcome);
  KJ_IF_SOME(i, info) {
    auto infoBuilder = builder.getInfo();
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, FetchResponseInfo) {
        fetch.copyTo(infoBuilder.initFetch());
      }
      KJ_CASE_ONEOF(tags, Tags) {
        auto list = infoBuilder.initCustom(tags.size());
        for (auto i: kj::indices(tags)) {
          tags[i].copyTo(list[i]);
        }
      }
    }
  }
}

Outcome Outcome::clone() const {
  return Outcome{outcome, cloneInfo(info)};
}

jsg::JsObject Outcome::toObject(jsg::Lock& js, NameProvider nameProvider) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("outcome"_kj));

  switch (outcome) {
    case EventOutcome::UNKNOWN:
      obj.set(js, "outcome", js.str("unknown"_kj));
      break;
    case EventOutcome::OK:
      obj.set(js, "outcome", js.str("ok"_kj));
      break;
    case EventOutcome::EXCEPTION:
      obj.set(js, "outcome", js.str("exception"_kj));
      break;
    case EventOutcome::EXCEEDED_CPU:
      obj.set(js, "outcome", js.str("exceeded-cpu"_kj));
      break;
    case EventOutcome::KILL_SWITCH:
      obj.set(js, "outcome", js.str("kill-switch"_kj));
      break;
    case EventOutcome::DAEMON_DOWN:
      obj.set(js, "outcome", js.str("daemon-down"_kj));
      break;
    case EventOutcome::SCRIPT_NOT_FOUND:
      obj.set(js, "outcome", js.str("script-not-found"_kj));
      break;
    case EventOutcome::CANCELED:
      obj.set(js, "outcome", js.str("canceled"_kj));
      break;
    case EventOutcome::EXCEEDED_MEMORY:
      obj.set(js, "outcome", js.str("exceeded-memory"_kj));
      break;
    case EventOutcome::LOAD_SHED:
      obj.set(js, "outcome", js.str("load-shed"_kj));
      break;
    case EventOutcome::RESPONSE_STREAM_DISCONNECTED:
      obj.set(js, "outcome", js.str("response-stream-disconnected"_kj));
      break;
  }

  KJ_IF_SOME(i, info) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, FetchResponseInfo) {
        obj.set(js, "info", fetch.toObject(js));
      }
      KJ_CASE_ONEOF(tags, Tags) {
        obj.set(js, "info", Tag::toObject(js, tags, nameProvider));
      }
    }
  }

  return obj;
}

// ======================================================================================
// FetchEventInfo

namespace {
kj::HttpMethod validateMethod(capnp::HttpMethod method) {
  KJ_REQUIRE(method <= capnp::HttpMethod::UNSUBSCRIBE, "unknown method", method);
  return static_cast<kj::HttpMethod>(method);
}
}  // namespace

FetchEventInfo::FetchEventInfo(
    kj::HttpMethod method, kj::String url, kj::String cfJson, kj::Array<Header> headers)
    : method(method),
      url(kj::mv(url)),
      cfJson(kj::mv(cfJson)),
      headers(kj::mv(headers)) {}

FetchEventInfo::FetchEventInfo(rpc::Trace::FetchEventInfo::Reader reader)
    : method(validateMethod(reader.getMethod())),
      url(kj::str(reader.getUrl())),
      cfJson(kj::str(reader.getCfJson())) {
  kj::Vector<Header> v;
  v.addAll(reader.getHeaders());
  headers = v.releaseAsArray();
}

void FetchEventInfo::copyTo(rpc::Trace::FetchEventInfo::Builder builder) const {
  builder.setMethod(static_cast<capnp::HttpMethod>(method));
  builder.setUrl(url);
  builder.setCfJson(cfJson);

  auto list = builder.initHeaders(headers.size());
  for (auto i: kj::indices(headers)) {
    headers[i].copyTo(list[i]);
  }
}

FetchEventInfo::Header::Header(kj::String name, kj::String value)
    : name(kj::mv(name)),
      value(kj::mv(value)) {}

FetchEventInfo::Header::Header(rpc::Trace::FetchEventInfo::Header::Reader reader)
    : name(kj::str(reader.getName())),
      value(kj::str(reader.getValue())) {}

void FetchEventInfo::Header::copyTo(rpc::Trace::FetchEventInfo::Header::Builder builder) const {
  builder.setName(name);
  builder.setValue(value);
}

FetchEventInfo FetchEventInfo::clone() const {
  kj::Vector<Header> newHeaders(headers.size());
  for (auto& header: headers) {
    newHeaders.add(Header(kj::str(header.name), kj::str(header.value)));
  }

  return FetchEventInfo(method, kj::str(url), kj::str(cfJson), newHeaders.releaseAsArray());
}

jsg::JsObject FetchEventInfo::toObject(jsg::Lock& js) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("fetch"_kj));

  obj.set(js, "method", js.str(kj::str(method)));
  obj.set(js, "url", js.str(url));
  obj.set(js, "cfJson", js.str(cfJson));

  if (headers.size() > 0) {
    auto headersObj = js.objNoProto();
    for (auto& header: headers) {
      headersObj.set(js, header.name, js.str(header.value));
    }
    obj.set(js, "headers", headersObj);
  }

  return obj;
}

// ======================================================================================
// FetchResponseInfo
FetchResponseInfo::FetchResponseInfo(uint16_t statusCode): statusCode(statusCode) {}

FetchResponseInfo::FetchResponseInfo(rpc::Trace::FetchResponseInfo::Reader reader)
    : statusCode(reader.getStatusCode()) {}

void FetchResponseInfo::copyTo(rpc::Trace::FetchResponseInfo::Builder builder) const {
  builder.setStatusCode(statusCode);
}

FetchResponseInfo FetchResponseInfo::clone() const {
  return FetchResponseInfo(statusCode);
}

jsg::JsObject FetchResponseInfo::toObject(jsg::Lock& js) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("fetch"_kj));
  obj.set(js, "statusCode", js.num(statusCode));
  return obj;
}

// ======================================================================================
// JsRpcEventInfo

JsRpcEventInfo::JsRpcEventInfo(kj::String methodName): methodName(kj::mv(methodName)) {}

JsRpcEventInfo::JsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Reader reader)
    : methodName(kj::str(reader.getMethodName())) {}

void JsRpcEventInfo::copyTo(rpc::Trace::JsRpcEventInfo::Builder builder) const {
  builder.setMethodName(methodName);
}

JsRpcEventInfo JsRpcEventInfo::clone() const {
  return JsRpcEventInfo(kj::str(methodName));
}

jsg::JsObject JsRpcEventInfo::toObject(jsg::Lock& js) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("jsrpc"_kj));
  obj.set(js, "methodName", js.str(methodName));
  return obj;
}

// ======================================================================================
// ScheduledEventInfo

ScheduledEventInfo::ScheduledEventInfo(double scheduledTime, kj::String cron)
    : scheduledTime(scheduledTime),
      cron(kj::mv(cron)) {}

ScheduledEventInfo::ScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTime()),
      cron(kj::str(reader.getCron())) {}

void ScheduledEventInfo::copyTo(rpc::Trace::ScheduledEventInfo::Builder builder) const {
  builder.setScheduledTime(scheduledTime);
  builder.setCron(cron);
}

ScheduledEventInfo ScheduledEventInfo::clone() const {
  return ScheduledEventInfo(scheduledTime, kj::str(cron));
}

jsg::JsObject ScheduledEventInfo::toObject(jsg::Lock& js) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("scheduled"_kj));
  obj.set(js, "scheduledTime", js.num(scheduledTime));
  obj.set(js, "cron", js.str(cron));
  return obj;
}

// ======================================================================================
// AlarmEventInfo

AlarmEventInfo::AlarmEventInfo(kj::Date scheduledTime): scheduledTime(scheduledTime) {}

AlarmEventInfo::AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTimeMs() * kj::MILLISECONDS + kj::UNIX_EPOCH) {}

void AlarmEventInfo::copyTo(rpc::Trace::AlarmEventInfo::Builder builder) const {
  builder.setScheduledTimeMs((scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
}

AlarmEventInfo AlarmEventInfo::clone() const {
  return AlarmEventInfo(scheduledTime);
}

jsg::JsObject AlarmEventInfo::toObject(jsg::Lock& js) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("alarm"_kj));
  obj.set(js, "scheduledTime", js.date(scheduledTime));
  return obj;
}

// ======================================================================================
// QueueEventInfo

QueueEventInfo::QueueEventInfo(kj::String queueName, uint32_t batchSize)
    : queueName(kj::mv(queueName)),
      batchSize(batchSize) {}

QueueEventInfo::QueueEventInfo(rpc::Trace::QueueEventInfo::Reader reader)
    : queueName(kj::heapString(reader.getQueueName())),
      batchSize(reader.getBatchSize()) {}

void QueueEventInfo::copyTo(rpc::Trace::QueueEventInfo::Builder builder) const {
  builder.setQueueName(queueName);
  builder.setBatchSize(batchSize);
}

QueueEventInfo QueueEventInfo::clone() const {
  return QueueEventInfo(kj::str(queueName), batchSize);
}

jsg::JsObject QueueEventInfo::toObject(jsg::Lock& js) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("queue"_kj));
  obj.set(js, "queueName", js.str(queueName));
  obj.set(js, "batchSize", js.num(batchSize));
  return obj;
}

// ======================================================================================
// EmailEventInfo

EmailEventInfo::EmailEventInfo(kj::String mailFrom, kj::String rcptTo, uint32_t rawSize)
    : mailFrom(kj::mv(mailFrom)),
      rcptTo(kj::mv(rcptTo)),
      rawSize(rawSize) {}

EmailEventInfo::EmailEventInfo(rpc::Trace::EmailEventInfo::Reader reader)
    : mailFrom(kj::heapString(reader.getMailFrom())),
      rcptTo(kj::heapString(reader.getRcptTo())),
      rawSize(reader.getRawSize()) {}

void EmailEventInfo::copyTo(rpc::Trace::EmailEventInfo::Builder builder) const {
  builder.setMailFrom(mailFrom);
  builder.setRcptTo(rcptTo);
  builder.setRawSize(rawSize);
}

EmailEventInfo EmailEventInfo::clone() const {
  return EmailEventInfo(kj::str(mailFrom), kj::str(rcptTo), rawSize);
}

jsg::JsObject EmailEventInfo::toObject(jsg::Lock& js) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("email"_kj));
  obj.set(js, "mailFrom", js.str(mailFrom));
  obj.set(js, "rcptTo", js.str(rcptTo));
  obj.set(js, "rawSize", js.num(rawSize));
  return obj;
}

// ======================================================================================
// HibernatableWebSocketEventInfo

HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(Type type): type(type) {}

HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(
    rpc::Trace::HibernatableWebSocketEventInfo::Reader reader)
    : type(readFrom(reader)) {}

void HibernatableWebSocketEventInfo::copyTo(
    rpc::Trace::HibernatableWebSocketEventInfo::Builder builder) const {
  auto typeBuilder = builder.initType();
  KJ_SWITCH_ONEOF(type) {
    KJ_CASE_ONEOF(_, Message) {
      typeBuilder.setMessage();
    }
    KJ_CASE_ONEOF(close, Close) {
      auto closeBuilder = typeBuilder.initClose();
      closeBuilder.setCode(close.code);
      closeBuilder.setWasClean(close.wasClean);
    }
    KJ_CASE_ONEOF(_, Error) {
      typeBuilder.setError();
    }
  }
}

HibernatableWebSocketEventInfo::Type HibernatableWebSocketEventInfo::readFrom(
    rpc::Trace::HibernatableWebSocketEventInfo::Reader reader) {
  auto type = reader.getType();
  switch (type.which()) {
    case rpc::Trace::HibernatableWebSocketEventInfo::Type::MESSAGE: {
      return Message{};
    }
    case rpc::Trace::HibernatableWebSocketEventInfo::Type::CLOSE: {
      auto close = type.getClose();
      return Close{
        .code = close.getCode(),
        .wasClean = close.getWasClean(),
      };
    }
    case rpc::Trace::HibernatableWebSocketEventInfo::Type::ERROR: {
      return Error{};
    }
  }
}

HibernatableWebSocketEventInfo HibernatableWebSocketEventInfo::clone() const {
  auto newType = ([this]() -> Type {
    KJ_SWITCH_ONEOF(type) {
      KJ_CASE_ONEOF(_, Message) {
        return Message{};
      }
      KJ_CASE_ONEOF(close, Close) {
        return Close{close.code, close.wasClean};
      }
      KJ_CASE_ONEOF(_, Error) {
        return Error{};
      }
    }
    KJ_UNREACHABLE;
  })();
  return HibernatableWebSocketEventInfo(kj::mv(newType));
}

jsg::JsObject HibernatableWebSocketEventInfo::toObject(jsg::Lock& js) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("hibernatable-websocket"_kj));

  KJ_SWITCH_ONEOF(type) {
    KJ_CASE_ONEOF(_, Message) {
      obj.set(js, "kind", js.str("message"_kj));
    }
    KJ_CASE_ONEOF(close, Close) {
      auto closeObj = js.obj();
      closeObj.set(js, "code", js.num(close.code));
      closeObj.set(js, "wasClean", js.boolean(close.wasClean));
      obj.set(js, "kind", closeObj);
    }
    KJ_CASE_ONEOF(_, Error) {
      obj.set(js, "kind", js.str("error"_kj));
    }
  }

  return obj;
}

// ======================================================================================
// TraceEventInfo

namespace {
kj::Vector<TraceEventInfo::TraceItem> getTraceItemsFromTraces(kj::ArrayPtr<kj::Own<Trace>> traces) {
  kj::Vector<TraceEventInfo::TraceItem> results(traces.size());
  for (size_t n = 0; n < traces.size(); n++) {
    KJ_IF_SOME(name, traces[n]->onsetInfo.scriptName) {
      TraceEventInfo::TraceItem item(kj::str(name));
      results.add(kj::mv(item));
    } else {
      TraceEventInfo::TraceItem item(kj::str("<unknown>"_kj));
      results.add(kj::mv(item));
    }
  }
  return results.releaseAsArray();
}

kj::Vector<TraceEventInfo::TraceItem> getTraceItemsFromReader(
    rpc::Trace::TraceEventInfo::Reader reader) {
  auto traces = reader.getTraces();
  kj::Vector<TraceEventInfo::TraceItem> results;
  results.addAll(traces);
  return results.releaseAsArray();
}
}  // namespace

TraceEventInfo::TraceEventInfo(kj::ArrayPtr<kj::Own<Trace>> traces)
    : traces(getTraceItemsFromTraces(traces)) {}

TraceEventInfo::TraceEventInfo(kj::Array<TraceItem> traces): traces(kj::mv(traces)) {}

TraceEventInfo::TraceEventInfo(rpc::Trace::TraceEventInfo::Reader reader)
    : traces(getTraceItemsFromReader(reader)) {}

void TraceEventInfo::copyTo(rpc::Trace::TraceEventInfo::Builder builder) const {
  auto list = builder.initTraces(traces.size());
  for (auto i: kj::indices(traces)) {
    traces[i].copyTo(list[i]);
  }
}

TraceEventInfo::TraceItem::TraceItem(kj::Maybe<kj::String> scriptName)
    : scriptName(kj::mv(scriptName)) {}

TraceEventInfo::TraceItem::TraceItem(rpc::Trace::TraceEventInfo::TraceItem::Reader reader)
    : scriptName(kj::str(reader.getScriptName())) {}

void TraceEventInfo::TraceItem::copyTo(
    rpc::Trace::TraceEventInfo::TraceItem::Builder builder) const {
  KJ_IF_SOME(name, scriptName) {
    builder.setScriptName(name);
  }
}

TraceEventInfo::TraceItem TraceEventInfo::TraceItem::clone() const {
  return TraceEventInfo::TraceItem(scriptName.map([](const kj::String& s) { return kj::str(s); }));
}

TraceEventInfo TraceEventInfo::clone() const {
  kj::Vector<TraceItem> newTraces(traces.size());
  for (auto& trace: traces) {
    newTraces.add(trace.clone());
  }
  return TraceEventInfo(newTraces.releaseAsArray());
}

jsg::JsObject TraceEventInfo::toObject(jsg::Lock& js) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("trace"_kj));

  kj::Vector<jsg::JsValue> vec(traces.size());
  for (auto& trace: traces) {
    KJ_IF_SOME(name, trace.scriptName) {
      vec.add(js.str(name));
    } else {
      vec.add(js.str("<unknown>"_kj));
    }
  }
  obj.set(js, "traces", js.arr(vec.releaseAsArray()));

  return obj;
}

// ======================================================================================
// DiagnosticChannelEvent

DiagnosticChannelEvent::DiagnosticChannelEvent(
    kj::Date timestamp, kj::String channel, kj::Array<kj::byte> message)
    : timestamp(timestamp),
      channel(kj::mv(channel)),
      message(kj::mv(message)) {}

DiagnosticChannelEvent::DiagnosticChannelEvent(rpc::Trace::DiagnosticChannelEvent::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      channel(kj::heapString(reader.getChannel())),
      message(kj::heapArray<kj::byte>(reader.getMessage())) {}

void DiagnosticChannelEvent::copyTo(rpc::Trace::DiagnosticChannelEvent::Builder builder) const {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setChannel(channel);
  builder.setMessage(message);
}

DiagnosticChannelEvent DiagnosticChannelEvent::clone() const {
  return DiagnosticChannelEvent(timestamp, kj::str(channel), kj::heapArray<kj::byte>(message));
}

jsg::JsObject DiagnosticChannelEvent::toObject(jsg::Lock& js) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("diagnostic-channel"_kj));
  obj.set(js, "timestamp", js.date(timestamp));
  obj.set(js, "channel", js.str(channel));

  jsg::Deserializer deser(js, message);
  obj.set(js, "message", deser.readValue(js));
  return obj;
}

// ======================================================================================
// Log

namespace {
kj::OneOf<kj::Array<kj::byte>, kj::String> getMessageForLog(
    const rpc::Trace::LogV2::Reader& reader) {
  auto message = reader.getMessage();
  switch (message.which()) {
    case rpc::Trace::LogV2::Message::Which::TEXT: {
      return kj::str(message.getText());
    }
    case rpc::Trace::LogV2::Message::Which::DATA: {
      return kj::heapArray<kj::byte>(message.getData());
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

Log::Log(kj::Date timestamp, LogLevel logLevel, kj::String message)
    : timestamp(timestamp),
      logLevel(logLevel),
      message(kj::mv(message)) {}

Log::Log(rpc::Trace::Log::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      logLevel(reader.getLogLevel()),
      message(kj::str(reader.getMessage())) {}

void Log::copyTo(rpc::Trace::Log::Builder builder) const {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setLogLevel(logLevel);
  builder.setMessage(message);
}

Log Log::clone() const {
  return Log(timestamp, logLevel, kj::str(message));
}

LogV2::LogV2(kj::Date timestamp,
    LogLevel logLevel,
    kj::OneOf<kj::Array<kj::byte>, kj::String> message,
    Tags tags,
    bool truncated)
    : timestamp(timestamp),
      logLevel(logLevel),
      message(kj::mv(message)),
      tags(kj::mv(tags)),
      truncated(truncated) {}

LogV2::LogV2(rpc::Trace::LogV2::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      logLevel(reader.getLogLevel()),
      message(getMessageForLog(reader)),
      truncated(reader.getTruncated()) {}

void LogV2::copyTo(rpc::Trace::LogV2::Builder builder) const {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setLogLevel(logLevel);
  KJ_SWITCH_ONEOF(message) {
    KJ_CASE_ONEOF(str, kj::String) {
      builder.initMessage().setText(str);
    }
    KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
      builder.initMessage().setData(data);
    }
  }
  builder.setTruncated(truncated);
  auto outTags = builder.initTags(tags.size());
  for (size_t n = 0; n < tags.size(); n++) {
    tags[n].copyTo(outTags[n]);
  }
}

LogV2 LogV2::clone() const {
  kj::Vector<Tag> newTags(tags.size());
  for (auto& tag: tags) {
    newTags.add(tag.clone());
  }
  auto newMessage = ([&]() -> kj::OneOf<kj::Array<kj::byte>, kj::String> {
    KJ_SWITCH_ONEOF(message) {
      KJ_CASE_ONEOF(str, kj::String) {
        return kj::str(str);
      }
      KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
        return kj::heapArray<kj::byte>(data);
      }
    }
    KJ_UNREACHABLE;
  })();
  return LogV2(timestamp, logLevel, kj::mv(newMessage), newTags.releaseAsArray(), truncated);
}

jsg::JsObject LogV2::toObject(jsg::Lock& js, NameProvider nameProvider) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("log"_kj));
  obj.set(js, "timestamp", js.date(timestamp));

  switch (logLevel) {
    case LogLevel::DEBUG_:
      obj.set(js, "logLevel", js.str("debug"_kj));
      break;
    case LogLevel::INFO:
      obj.set(js, "logLevel", js.str("info"_kj));
      break;
    case LogLevel::WARN:
      obj.set(js, "logLevel", js.str("warn"_kj));
      break;
    case LogLevel::ERROR:
      obj.set(js, "logLevel", js.str("error"_kj));
      break;
    case LogLevel::LOG:
      obj.set(js, "logLevel", js.str("log"_kj));
      break;
  }

  KJ_SWITCH_ONEOF(message) {
    KJ_CASE_ONEOF(str, kj::String) {
      obj.set(js, "message", js.str(str));
    }
    KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
      jsg::Deserializer deser(js, data);
      obj.set(js, "message", deser.readValue(js));
    }
  }
  obj.set(js, "truncated", js.boolean(truncated));
  obj.set(js, "tags", Tag::toObject(js, tags, nameProvider, Tag::ToObjectOptions::UNWRAPPED));
  return obj;
}

// ======================================================================================
// Exception
namespace {
kj::Maybe<kj::String> maybeGetStack(const rpc::Trace::Exception::Reader& reader) {
  if (!reader.hasStack()) return kj::none;
  return kj::str(reader.getStack());
}
Exception::Detail getDetail(const rpc::Trace::Exception::Reader& reader) {
  auto detailReader = reader.getDetail();
  Exception::Detail detail;
  if (detailReader.hasCause()) {
    detail.cause = kj::heap(Exception(detailReader.getCause()));
  }
  if (detailReader.hasErrors()) {
    kj::Vector<kj::Own<Exception>> errors(detailReader.getErrors().size());
    for (auto error: detailReader.getErrors()) {
      errors.add(kj::heap(Exception(error)));
    }
    detail.errors = errors.releaseAsArray();
  }
  if (detailReader.hasTags()) {
    detail.tags = getTagsFromReader(detailReader.getTags());
  }
  detail.retryable = detailReader.getRetryable();
  detail.remote = detailReader.getRemote();
  detail.overloaded = detailReader.getOverloaded();
  detail.durableObjectReset = detailReader.getDurableObjectReset();
  return kj::mv(detail);
}
}  // namespace

Exception::Detail Exception::Detail::clone() const {
  auto newCause = ([&]() -> kj::Maybe<kj::Own<Exception>> {
    KJ_IF_SOME(exception, cause) {
      return kj::Maybe(kj::heap(exception->clone()));
    }
    return kj::none;
  })();
  auto newErrors = ([&]() -> kj::Array<kj::Own<Exception>> {
    kj::Vector<kj::Own<Exception>> results(errors.size());
    for (auto& error: errors) {
      results.add(kj::heap(error->clone()));
    }
    return results.releaseAsArray();
  })();
  auto newTags = ([&]() -> Tags {
    kj::Vector<Tag> results(tags.size());
    for (auto& tag: tags) {
      results.add(tag.clone());
    }
    return results.releaseAsArray();
  })();
  return Detail{
    .cause = kj::mv(newCause),
    .errors = kj::mv(newErrors),
    .remote = remote,
    .retryable = retryable,
    .overloaded = overloaded,
    .durableObjectReset = durableObjectReset,
    .tags = kj::mv(newTags),
  };
}

Exception::Exception(kj::Date timestamp,
    kj::String name,
    kj::String message,
    kj::Maybe<kj::String> stack,
    kj::Maybe<Detail> detail)
    : timestamp(timestamp),
      name(kj::mv(name)),
      message(kj::mv(message)),
      stack(kj::mv(stack)),
      detail(kj::mv(detail).orDefault({})) {}

Exception::Exception(rpc::Trace::Exception::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      name(kj::str(reader.getName())),
      message(kj::str(reader.getMessage())),
      stack(maybeGetStack(reader)),
      detail(getDetail(reader)) {}

void Exception::copyTo(rpc::Trace::Exception::Builder builder) const {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setName(name);
  builder.setMessage(message);
  KJ_IF_SOME(s, stack) {
    builder.setStack(s);
  }

  auto detailBuilder = builder.initDetail();
  KJ_IF_SOME(cause, detail.cause) {
    cause->copyTo(detailBuilder.initCause());
  }
  if (detail.errors.size() > 0) {
    auto errorsBuilder = detailBuilder.initErrors(detail.errors.size());
    for (size_t n = 0; n < detail.errors.size(); n++) {
      detail.errors[n]->copyTo(errorsBuilder[n]);
    }
  }
  detailBuilder.setRemote(detail.remote);
  detailBuilder.setRetryable(detail.retryable);
  detailBuilder.setOverloaded(detail.overloaded);
  detailBuilder.setDurableObjectReset(detail.durableObjectReset);
  if (detail.tags.size() > 0) {
    auto tagsBuilder = detailBuilder.initTags(detail.tags.size());
    for (size_t n = 0; n < detail.tags.size(); n++) {
      detail.tags[n].copyTo(tagsBuilder[n]);
    }
  }
}

Exception Exception::clone() const {
  return Exception(timestamp, kj::str(name), kj::str(message),
      stack.map([](const kj::String& s) { return kj::str(s); }), detail.clone());
}

jsg::JsObject Exception::toObject(jsg::Lock& js, NameProvider nameProvider) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("exception"_kj));
  obj.set(js, "timestamp", js.date(timestamp));
  obj.set(js, "name", js.str(name));
  obj.set(js, "message", js.str(message));
  KJ_IF_SOME(s, stack) {
    obj.set(js, "stack", js.str(s));
  }

  obj.set(js, "remote", js.boolean(detail.remote));
  obj.set(js, "retryable", js.boolean(detail.retryable));
  obj.set(js, "overloaded", js.boolean(detail.overloaded));
  obj.set(js, "durableObjectReset", js.boolean(detail.durableObjectReset));
  obj.set(
      js, "tags", Tag::toObject(js, detail.tags, nameProvider, Tag::ToObjectOptions::UNWRAPPED));

  return obj;
}

// ======================================================================================
// Subrequest
namespace {
kj::Maybe<Subrequest::Info> maybeGetSubrequestInfo(const rpc::Trace::Subrequest::Reader& reader) {
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::Subrequest::Info::Which::NONE: {
      return kj::none;
    }
    case rpc::Trace::Subrequest::Info::Which::FETCH: {
      return kj::Maybe(FetchEventInfo(info.getFetch()));
    }
    case rpc::Trace::Subrequest::Info::Which::JS_RPC: {
      return kj::Maybe(JsRpcEventInfo(info.getJsRpc()));
    }
    case rpc::Trace::Subrequest::Info::Which::CUSTOM: {
      auto custom = info.getCustom();
      kj::Vector<Tag> tags(custom.size());
      for (auto c: custom) {
        tags.add(Tag(c));
      }
      return kj::Maybe(tags.releaseAsArray());
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

Subrequest::Subrequest(uint32_t id, kj::Maybe<Info> info): id(id), info(kj::mv(info)) {}

Subrequest::Subrequest(rpc::Trace::Subrequest::Reader reader)
    : id(reader.getId()),
      info(maybeGetSubrequestInfo(reader)) {}

void Subrequest::copyTo(rpc::Trace::Subrequest::Builder builder) const {
  builder.setId(id);
  auto infoBuilder = builder.initInfo();
  KJ_IF_SOME(i, info) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, FetchEventInfo) {
        fetch.copyTo(infoBuilder.initFetch());
      }
      KJ_CASE_ONEOF(jsRpc, JsRpcEventInfo) {
        jsRpc.copyTo(infoBuilder.initJsRpc());
      }
      KJ_CASE_ONEOF(custom, Tags) {
        auto customBuilder = infoBuilder.initCustom(custom.size());
        for (size_t n = 0; n < custom.size(); n++) {
          custom[n].copyTo(customBuilder[n]);
        }
      }
    }
  }
}

Subrequest Subrequest::clone() const {
  auto newInfo = ([&]() -> kj::Maybe<Info> {
    KJ_IF_SOME(i, info) {
      KJ_SWITCH_ONEOF(i) {
        KJ_CASE_ONEOF(fetch, FetchEventInfo) {
          return kj::Maybe(fetch.clone());
        }
        KJ_CASE_ONEOF(jsRpc, JsRpcEventInfo) {
          return kj::Maybe(jsRpc.clone());
        }
        KJ_CASE_ONEOF(custom, Tags) {
          kj::Vector<Tag> newTags(custom.size());
          for (auto& tag: custom) {
            newTags.add(tag.clone());
          }
          return kj::Maybe(newTags.releaseAsArray());
        }
      }
      KJ_UNREACHABLE;
    }
    return kj::none;
  })();
  return Subrequest(id, kj::mv(newInfo));
}

jsg::JsObject Subrequest::toObject(jsg::Lock& js, NameProvider nameProvider) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("subrequest"_kj));
  obj.set(js, "id", js.num(id));

  KJ_IF_SOME(i, info) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, FetchEventInfo) {
        obj.set(js, "info", fetch.toObject(js));
      }
      KJ_CASE_ONEOF(jsRpc, JsRpcEventInfo) {
        obj.set(js, "info", jsRpc.toObject(js));
      }
      KJ_CASE_ONEOF(tags, Tags) {
        obj.set(js, "info", Tag::toObject(js, tags, nameProvider));
      }
    }
  }

  return obj;
}

// ======================================================================================
// SubrequestOutcome
namespace {
kj::Maybe<SubrequestOutcome::Info> maybeGetSubrequestOutcome(
    const rpc::Trace::SubrequestOutcome::Reader& reader) {
  auto info = reader.getInfo();
  switch (info.which()) {
    case rpc::Trace::SubrequestOutcome::Info::Which::NONE: {
      return kj::none;
    }
    case rpc::Trace::SubrequestOutcome::Info::Which::FETCH: {
      return kj::Maybe(FetchResponseInfo(info.getFetch()));
    }
    case rpc::Trace::SubrequestOutcome::Info::Which::CUSTOM: {
      return kj::Maybe(getTagsFromReader(info.getCustom()));
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace
SubrequestOutcome::SubrequestOutcome(uint32_t id, kj::Maybe<Info> info, SpanClose::Outcome outcome)
    : id(id),
      info(kj::mv(info)),
      outcome(outcome) {}

SubrequestOutcome::SubrequestOutcome(rpc::Trace::SubrequestOutcome::Reader reader)
    : id(reader.getId()),
      info(maybeGetSubrequestOutcome(reader)),
      outcome(reader.getOutcome()) {}

void SubrequestOutcome::copyTo(rpc::Trace::SubrequestOutcome::Builder builder) const {
  builder.setId(id);
  builder.setOutcome(outcome);
  KJ_IF_SOME(i, info) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, FetchResponseInfo) {
        fetch.copyTo(builder.getInfo().initFetch());
      }
      KJ_CASE_ONEOF(tags, Tags) {
        auto custom = builder.getInfo().initCustom(tags.size());
        for (size_t n = 0; n < tags.size(); n++) {
          tags[n].copyTo(custom[n]);
        }
      }
    }
  }
}

SubrequestOutcome SubrequestOutcome::clone() const {
  auto newInfo = ([&]() -> kj::Maybe<Info> {
    KJ_IF_SOME(i, info) {
      KJ_SWITCH_ONEOF(i) {
        KJ_CASE_ONEOF(fetch, FetchResponseInfo) {
          return kj::Maybe(fetch.clone());
        }
        KJ_CASE_ONEOF(tags, Tags) {
          kj::Vector<Tag> newTags(tags.size());
          for (auto& tag: tags) {
            newTags.add(tag.clone());
          }
          return kj::Maybe(newTags.releaseAsArray());
        }
      }
    }
    return kj::none;
  })();
  return SubrequestOutcome(id, kj::mv(newInfo), outcome);
}

jsg::JsObject SubrequestOutcome::toObject(jsg::Lock& js, NameProvider nameProvider) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("subrequest-outcome"_kj));
  obj.set(js, "id", js.num(id));

  switch (outcome) {
    case SpanClose::Outcome::OK:
      obj.set(js, "outcome", js.str("ok"_kj));
      break;
    case SpanClose::Outcome::EXCEPTION:
      obj.set(js, "outcome", js.str("exception"_kj));
      break;
    case SpanClose::Outcome::CANCELED:
      obj.set(js, "outcome", js.str("canceled"_kj));
      break;
    case SpanClose::Outcome::UNKNOWN:
      obj.set(js, "outcome", js.str("unknown"_kj));
      break;
  }

  KJ_IF_SOME(i, info) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(fetch, FetchResponseInfo) {
        obj.set(js, "info", fetch.toObject(js));
      }
      KJ_CASE_ONEOF(tags, Tags) {
        obj.set(js, "info", Tag::toObject(js, tags, nameProvider));
      }
    }
  }

  return obj;
}

// ======================================================================================
// SpanClose

SpanClose::SpanClose(Outcome outcome, Tags tags): outcome(outcome), tags(kj::mv(tags)) {}

SpanClose::SpanClose(rpc::Trace::SpanClose::Reader reader)
    : outcome(reader.getOutcome()),
      tags(maybeGetTags(reader)) {}

void SpanClose::copyTo(rpc::Trace::SpanClose::Builder builder) const {
  builder.setOutcome(outcome);
  auto outTags = builder.initTags(tags.size());
  for (size_t n = 0; n < tags.size(); n++) {
    tags[n].copyTo(outTags[n]);
  }
}

SpanClose SpanClose::clone() const {
  return SpanClose(outcome, KJ_MAP(tag, tags) { return tag.clone(); });
}

jsg::JsObject SpanClose::toObject(jsg::Lock& js, NameProvider nameProvider) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("span"_kj));

  switch (outcome) {
    case Outcome::OK:
      obj.set(js, "outcome", js.str("ok"_kj));
      break;
    case Outcome::EXCEPTION:
      obj.set(js, "outcome", js.str("exception"_kj));
      break;
    case Outcome::CANCELED:
      obj.set(js, "outcome", js.str("canceled"_kj));
      break;
    case Outcome::UNKNOWN:
      obj.set(js, "outcome", js.str("unknown"_kj));
      break;
  }

  obj.set(js, "tags", Tag::toObject(js, tags, nameProvider, Tag::ToObjectOptions::UNWRAPPED));
  return obj;
}

// ======================================================================================
// Mark

Mark::Mark(kj::String name): name(kj::mv(name)) {}

Mark::Mark(rpc::Trace::Mark::Reader reader): name(kj::str(reader.getName())) {}

void Mark::copyTo(rpc::Trace::Mark::Builder builder) const {
  builder.setName(name);
}

Mark Mark::clone() const {
  return Mark(kj::str(name));
}

jsg::JsObject Mark::toObject(jsg::Lock& js) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("mark"_kj));
  obj.set(js, "name", js.str(name));
  return obj;
}

// ======================================================================================
// Metric
namespace {
Metric::Key getMetricKey(const rpc::Trace::Metric::Reader& reader) {
  auto key = reader.getKey();
  switch (key.which()) {
    case rpc::Trace::Metric::Key::Which::TEXT: {
      return kj::str(key.getText());
    }
    case rpc::Trace::Metric::Key::Which::ID: {
      return key.getId();
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

Metric::Metric(Type type, Key key, double value)
    : type(type),
      key(kj::mv(key)),
      value(kj::mv(value)) {}

Metric::Metric(rpc::Trace::Metric::Reader reader)
    : type(reader.getType()),
      key(getMetricKey(reader)),
      value(reader.getValue()) {}

void Metric::copyTo(rpc::Trace::Metric::Builder builder) const {
  builder.setType(type);
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(str, kj::String) {
      builder.getKey().setText(str);
    }
    KJ_CASE_ONEOF(id, uint32_t) {
      builder.getKey().setId(id);
    }
  }
  builder.setValue(value);
}

Metric Metric::clone() const {
  auto newKey = ([&]() -> Key {
    KJ_SWITCH_ONEOF(key) {
      KJ_CASE_ONEOF(str, kj::String) {
        return kj::str(str);
      }
      KJ_CASE_ONEOF(id, uint32_t) {
        return id;
      }
    }
    KJ_UNREACHABLE;
  })();
  return Metric(type, kj::mv(newKey), value);
}

bool Metric::keyMatches(kj::OneOf<kj::StringPtr, uint32_t> check) {
  KJ_SWITCH_ONEOF(check) {
    KJ_CASE_ONEOF(c, kj::StringPtr) {
      KJ_IF_SOME(k, key.tryGet<kj::String>()) {
        return k == c;
      }
    }
    KJ_CASE_ONEOF(u, uint32_t) {
      KJ_IF_SOME(k, key.tryGet<uint32_t>()) {
        return k == u;
      }
    }
  }
  return false;
}

jsg::JsObject Metric::toObject(
    jsg::Lock& js, kj::ArrayPtr<const Metric> metrics, NameProvider nameProvider) {
  auto obj = js.obj();
  obj.set(js, "type", js.str("metrics"_kj));

  auto counters = js.obj();
  auto gauges = js.obj();
  obj.set(js, "counters", counters);
  obj.set(js, "gauges", gauges);

  for (auto& metric: metrics) {
    auto maybeName = ([&]() -> kj::Maybe<kj::StringPtr> {
      KJ_SWITCH_ONEOF(metric.key) {
        KJ_CASE_ONEOF(str, kj::String) {
          return str.asPtr();
        }
        KJ_CASE_ONEOF(id, uint32_t) {
          return nameProvider(id, NameProviderContext::METRIC);
        }
      }
      return kj::none;
    })();

    auto container = metric.type == Type::COUNTER ? counters : gauges;

    auto value = js.num(metric.value);
    KJ_IF_SOME(name, maybeName) {
      if (container.has(js, name)) {
        auto existing = container.get(js, name);
        KJ_IF_SOME(arr, existing.tryCast<jsg::JsArray>()) {
          arr.add(js, value);
        } else {
          container.set(js, name, js.arr(existing, value));
        }
      } else {
        // The name does not currently exist in the object.
        container.set(js, name, value);
      }
    }
  }

  return obj;
}

// ======================================================================================
// Dropped

Dropped::Dropped(uint32_t start, uint32_t end): start(start), end(end) {}

Dropped::Dropped(rpc::Trace::Dropped::Reader reader)
    : start(reader.getStart()),
      end(reader.getEnd()) {}

void Dropped::copyTo(rpc::Trace::Dropped::Builder builder) const {
  builder.setStart(start);
  builder.setEnd(end);
}

Dropped Dropped::clone() const {
  return Dropped(start, end);
}

jsg::JsObject Dropped::toObject(jsg::Lock& js) const {
  auto obj = js.obj();
  obj.set(js, "type", js.str("dropped"_kj));
  obj.set(js, "start", js.num(start));
  obj.set(js, "end", js.num(end));
  return obj;
}

}  // namespace workerd::trace
