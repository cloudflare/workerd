// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "blob.h"
#include <kj/vector.h>
#include <workerd/jsg/jsg.h>
#include <workerd/io/compatibility-date.capnp.h>

namespace workerd::api {

// Implements the FormData interface as prescribed by:
// https://xhr.spec.whatwg.org/#interface-formdata
//
// NOTE: This class is actually reused by some internal code implementing the fiddle service, for
//   lack of any other C++ form data parser implementation. In that usage, there is no isolate.
//   It uses `parse()` and `getData()`. This relies on the ability to construct `File` objects
//   without an isolate.
class FormData: public jsg::Object {
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

    JSG_MEMORY_INFO(IteratorState) {
      tracker.trackField("parent", parent);
    }
  };

public:
  // Given a delimiter string `boundary`, serialize all fields in this form data to an array of
  // bytes suitable for use as an HTTP message body.
  kj::Array<kj::byte> serialize(kj::ArrayPtr<const char> boundary);

  // Parse `rawText`, storing the results in this FormData object. `contentType` must be either
  // multipart/form-data or application/x-www-form-urlencoded.
  //
  // `convertFilesToStrings` is for backwards-compatibility. The first implementation of this
  // class in Workers incorrectly represented files as strings (of their content). Changing this
  // could break deployed code, so this has to be controlled by a compatibility flag.
  //
  // Parsing may or may not pass a jsg::Lock. If a lock is passed, any File objects created will
  // track their internal allocated memory in the associated isolate. If a lock is not passed,
  // the internal allocated memory will not be tracked.
  void parse(kj::Maybe<jsg::Lock&> js, kj::ArrayPtr<const char> rawText,
             kj::StringPtr contentType, bool convertFilesToStrings);

  struct Entry {
    kj::String name;
    kj::OneOf<jsg::Ref<File>, kj::String> value;

    JSG_MEMORY_INFO(Entry) {
      tracker.trackField("name", name);
      KJ_SWITCH_ONEOF(value) {
        KJ_CASE_ONEOF(file, jsg::Ref<File>) {
          tracker.trackField("value", file);
        }
        KJ_CASE_ONEOF(str, kj::String) {
          tracker.trackField("value", str);
        }
      }
    }
  };

  kj::ArrayPtr<const Entry> getData() { return data; }

  // JS API

  // The spec allows a FormData to be constructed from a <form> HTML element. We don't support that,
  // for obvious reasons, so this constructor doesn't take any parameters. If someone tries to use
  // FormData to represent a <form> element we probably don't have to worry about making the error
  // message they receive too pretty: they won't get farther than `document.getElementById()`.
  static jsg::Ref<FormData> constructor();

  void append(jsg::Lock& js, kj::String name,
              kj::OneOf<jsg::Ref<File>, jsg::Ref<Blob>, kj::String> value,
              jsg::Optional<kj::String> filename);

  void delete_(kj::String name);

  kj::Maybe<kj::OneOf<jsg::Ref<File>, kj::String>> get(kj::String name);

  kj::Array<kj::OneOf<jsg::Ref<File>, kj::String>> getAll(kj::String name);

  bool has(kj::String name);

  void set(jsg::Lock& js, kj::String name,
           kj::OneOf<jsg::Ref<File>, jsg::Ref<Blob>, kj::String> value,
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
      jsg::Function<void(EntryType, kj::StringPtr, jsg::Ref<FormData>)> callback,
      jsg::Optional<jsg::Value> thisArg);

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

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("data", data.asPtr());
  }

private:
  kj::Vector<Entry> data;

  static EntryType clone(EntryType& value);

  template <typename Type>
  static kj::Maybe<Type> iteratorNext(jsg::Lock& js, IteratorState& state) {
    if (state.index >= state.parent->data.size()) {
      return kj::none;
    }
    auto& [key, value] = state.parent->data[state.index++];
    if constexpr (kj::isSameType<Type, EntryIteratorType>()) {
      return kj::arr<EntryType>(kj::str(key), clone(value));
    } else if constexpr (kj::isSameType<Type, KeyIteratorType>()) {
      return kj::str(key);
    } else if constexpr (kj::isSameType<Type, ValueIteratorType>()) {
      return clone(value);
    } else {
      KJ_UNREACHABLE;
    }
  }
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
