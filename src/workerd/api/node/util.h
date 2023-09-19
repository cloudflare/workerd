// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/util/mimetype.h>

namespace workerd::api::node {

class MIMEType;

class MIMEParams final: public jsg::Object {
private:
  template <typename T>
  struct IteratorState final {
    kj::Array<T> values;
    uint index = 0;
  };

public:
  MIMEParams(kj::Maybe<MimeType&> mimeType = kj::none);

  static jsg::Ref<MIMEParams> constructor();

  void delete_(kj::String name);
  kj::Maybe<kj::StringPtr> get(kj::String name);
  bool has(kj::String name);
  void set(kj::String name, kj::String value);
  kj::String toString();

  JSG_ITERATOR(EntryIterator, entries,
               kj::Array<kj::String>,
               IteratorState<kj::Array<kj::String>>,
               iteratorNext<kj::Array<kj::String>>);
  JSG_ITERATOR(KeyIterator, keys,
                kj::String,
                IteratorState<kj::String>,
                iteratorNext<kj::String>);
  JSG_ITERATOR(ValueIterator,
                values,
                kj::String,
                IteratorState<kj::String>,
                iteratorNext<kj::String>);

  JSG_RESOURCE_TYPE(MIMEParams) {
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(get);
    JSG_METHOD(has);
    JSG_METHOD(set);
    JSG_METHOD(entries);
    JSG_METHOD(keys);
    JSG_METHOD(values);
    JSG_METHOD(toString);
    JSG_METHOD_NAMED(toJSON, toString);
    JSG_ITERABLE(entries);
  }

private:
  template <typename T>
  static kj::Maybe<T> iteratorNext(jsg::Lock& js, IteratorState<T>& state) {
    if (state.index >= state.values.size()) {
      return kj::none;
    }
    auto& item = state.values[state.index++];
    if constexpr (kj::isSameType<T, kj::Array<kj::String>>()) {
      return KJ_MAP(i, item) { return kj::str(i); };
    } else {
      static_assert(kj::isSameType<T, kj::String>());
      return kj::str(item);
    }
    KJ_UNREACHABLE;
  }

  kj::Maybe<MimeType&> mimeType;
  friend class MIMEType;
};

class MIMEType final: public jsg::Object {
public:
  MIMEType(MimeType inner);
  ~MIMEType() noexcept(false);
  static jsg::Ref<MIMEType> constructor(kj::String input);

  kj::StringPtr getType();
  void setType(kj::String type);
  kj::StringPtr getSubtype();
  void setSubtype(kj::String subtype);
  kj::String getEssence();
  jsg::Ref<MIMEParams> getParams();
  kj::String toString();

  JSG_RESOURCE_TYPE(MIMEType) {
    JSG_PROTOTYPE_PROPERTY(type, getType, setType);
    JSG_PROTOTYPE_PROPERTY(subtype, getSubtype, setSubtype);
    JSG_READONLY_PROTOTYPE_PROPERTY(essence, getEssence);
    JSG_READONLY_PROTOTYPE_PROPERTY(params, getParams);
    JSG_METHOD(toString);
    JSG_METHOD_NAMED(toJSON, toString);
  }

private:
  workerd::MimeType inner;
  jsg::Ref<MIMEParams> params;
};

class UtilModule final: public jsg::Object {
public:

  JSG_RESOURCE_TYPE(UtilModule) {
    JSG_NESTED_TYPE(MIMEType);
    JSG_NESTED_TYPE(MIMEParams);
  }
};

#define EW_NODE_UTIL_ISOLATE_TYPES              \
    api::node::UtilModule,                      \
    api::node::MIMEType,                        \
    api::node::MIMEParams,                      \
    api::node::MIMEParams::EntryIterator,       \
    api::node::MIMEParams::ValueIterator,       \
    api::node::MIMEParams::KeyIterator,         \
    api::node::MIMEParams::EntryIterator::Next, \
    api::node::MIMEParams::ValueIterator::Next, \
    api::node::MIMEParams::KeyIterator::Next

}  // namespace workerd::api::node
