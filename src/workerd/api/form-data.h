// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/vector.h>
#include <kj/compat/url.h>
#include <workerd/jsg/jsg.h>
#include "blob.h"
#include <workerd/io/compatibility-date.capnp.h>

namespace workerd::api {

namespace url {
class URLSearchParams;
}  // namespace url

class FormData: public jsg::Object {
  // Implements the FormData interface as prescribed by:
  // https://xhr.spec.whatwg.org/#interface-formdata
  //
  // NOTE: This class is actually reused by some internal code implementing the fiddle service, for
  //   lack of any other C++ form data parser implementation. In that usage, there is no isolate.
  //   It uses `parse()` and `getData()`. This relies on the ability to construct `File` objects
  //   without an isolate.
private:
  using EntryType = kj::OneOf<jsg::Ref<File>, kj::String>;
  using EntryIteratorType = kj::Array<EntryType>;
  using KeyIteratorType = kj::String;
  using ValueIteratorType = EntryType;

  struct IteratorState final {
    jsg::Ref<FormData> parent;
    uint index = 0;

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(parent);
    }
  };

public:
  kj::Array<kj::byte> serialize(kj::ArrayPtr<const char> boundary);
  // Given a delimiter string `boundary`, serialize all fields in this form data to an array of
  // bytes suitable for use as an HTTP message body.

  void parse(kj::ArrayPtr<const char> rawText, kj::StringPtr contentType,
             bool convertFilesToStrings);
  // Parse `rawText`, storing the results in this FormData object. `contentType` must be either
  // multipart/form-data or application/x-www-form-urlencoded.
  //
  // `convertFilesToStrings` is for backwards-compatibility. The first implementation of this
  // class in Workers incorrectly represented files as strings (of their content). Changing this
  // could break deployed code, so this has to be controlled by a compatibility flag.

  struct Entry {
    kj::String name;
    kj::OneOf<jsg::Ref<File>, kj::String> value;
  };

  kj::ArrayPtr<const Entry> getData() { return data; }

  // JS API

  static jsg::Ref<FormData> constructor();
  // The spec allows a FormData to be constructed from a <form> HTML element. We don't support that,
  // for obvious reasons, so this constructor doesn't take any parameters. If someone tries to use
  // FormData to represent a <form> element we probably don't have to worry about making the error
  // message they receive too pretty: they won't get farther than `document.getElementById()`.

  void append(kj::String name, kj::OneOf<jsg::Ref<File>, jsg::Ref<Blob>, kj::String> value,
              jsg::Optional<kj::String> filename);

  void delete_(kj::String name);

  kj::Maybe<kj::OneOf<jsg::Ref<File>, kj::String>> get(kj::String name, v8::Isolate* isolate);

  kj::Array<kj::OneOf<jsg::Ref<File>, kj::String>> getAll(kj::String name, v8::Isolate* isolate);

  bool has(kj::String name);

  void set(kj::String name, kj::OneOf<jsg::Ref<File>, jsg::Ref<Blob>, kj::String> value,
           jsg::Optional<kj::String> filename);

  JSG_ITERATOR(EntryIterator, entries,
                EntryIteratorType,
                IteratorState,
                iteratorNext<EntryIteratorType>);
  JSG_ITERATOR(KeyIterator, keys,
                KeyIteratorType,
                IteratorState,
                iteratorNext<KeyIteratorType>);
  JSG_ITERATOR(ValueIterator,
                values,
                ValueIteratorType,
                IteratorState,
                iteratorNext<ValueIteratorType>);

  void forEach(
      jsg::Lock& js,
      jsg::V8Ref<v8::Function> callback,
      jsg::Optional<jsg::Value> thisArg,
      const jsg::TypeHandler<EntryType>& handler);

  JSG_RESOURCE_TYPE(FormData, CompatibilityFlags::Reader flags) {
    JSG_METHOD(append);
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(get);
    JSG_METHOD(getAll);
    JSG_METHOD(has);
    JSG_METHOD(set);
    JSG_METHOD(entries);
    JSG_METHOD(keys);
    JSG_METHOD(values);

    JSG_METHOD(forEach);
    JSG_ITERABLE(entries);

    if (flags.getFormDataParserSupportsFiles()) {
      JSG_TS_OVERRIDE({
        append(name: string, value: string): void;
        append(name: string, value: Blob, filename?: string): void;

        set(name: string, value: string): void;
        set(name: string, value: Blob, filename?: string): void;

        entries(): IterableIterator<[key: string, value: File | string]>;
        [Symbol.iterator](): IterableIterator<[key: string, value: File | string]>;

        forEach<This = unknown>(callback: (this: This, value: File | string, key: string, parent: FormData) => void, thisArg?: This): void;
      });
    } else {
      JSG_TS_OVERRIDE({
        get(name: string): string | null;
        getAll(name: string): string[];

        append(name: string, value: string): void;
        append(name: string, value: Blob, filename?: string): void;

        set(name: string, value: string): void;
        set(name: string, value: Blob, filename?: string): void;

        entries(): IterableIterator<[key: string, value: string]>;
        [Symbol.iterator](): IterableIterator<[key: string, value: string]>;

        forEach<This = unknown>(callback: (this: This, value: string, key: string, parent: FormData) => void, thisArg?: This): void;
      });
    }
  }

private:
  kj::Vector<Entry> data;

  static EntryType clone(v8::Isolate* isolate, EntryType& value);

  template <typename Type>
  static kj::Maybe<Type> iteratorNext(jsg::Lock& js, IteratorState& state) {
    if (state.index >= state.parent->data.size()) {
      return nullptr;
    }
    auto& [key, value] = state.parent->data[state.index++];
    if constexpr (kj::isSameType<Type, EntryIteratorType>()) {
      return kj::arr<EntryType>(kj::str(key), clone(js.v8Isolate, value));
    } else if constexpr (kj::isSameType<Type, KeyIteratorType>()) {
      return kj::str(key);
    } else if constexpr (kj::isSameType<Type, ValueIteratorType>()) {
      return clone(js.v8Isolate, value);
    } else {
      KJ_UNREACHABLE;
    }
  }

  friend class url::URLSearchParams;
};

#define EW_FORMDATA_ISOLATE_TYPES     \
  api::FormData,                      \
  api::FormData::EntryIterator,       \
  api::FormData::EntryIterator::Next, \
  api::FormData::KeyIterator,         \
  api::FormData::KeyIterator::Next,   \
  api::FormData::ValueIterator,       \
  api::FormData::ValueIterator::Next

}  // namespace workerd::api
