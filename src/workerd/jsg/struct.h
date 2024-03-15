// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// Translates between C++ struct types and JavaScript objects. This translation is by value: the
// struct is translated to/from a native JS object with the same field names.

#include "util.h"
#include "value.h"
#include "web-idl.h"

namespace workerd::jsg {

template <typename TypeWrapper, typename Struct, typename T,
          T Struct::*field, const char* name, size_t namePrefixStripLength>
class FieldWrapper {
  static constexpr inline const char* exportedName = name + namePrefixStripLength;
public:
  using Type = T;

  explicit FieldWrapper(v8::Isolate* isolate)
      : nameHandle(isolate, v8StrIntern(isolate, exportedName)) {}

  void wrap(TypeWrapper& wrapper, v8::Isolate* isolate, v8::Local<v8::Context> context,
            kj::Maybe<v8::Local<v8::Object>> creator, Struct& in, v8::Local<v8::Object> out) {
    if constexpr (kj::isSameType<T, SelfRef>()) {
      // Ignore SelfRef when converting to JS.
    } else if constexpr (kj::isSameType<T, Unimplemented>() || kj::isSameType<T, WontImplement>()) {
      // Fields with these types are required NOT to be present, so don't try to convert them.
    } else {
      if constexpr (webidl::isOptional<Type>) {
        // Don't even set optional fields that aren't present.
        if (in.*field == kj::none) return;
      }
      auto value = wrapper.wrap(context, creator, kj::mv(in.*field));
      check(out->Set(context, nameHandle.Get(isolate), value));
    }
  }

  Type unwrap(TypeWrapper& wrapper, v8::Isolate* isolate, v8::Local<v8::Context> context,
              v8::Local<v8::Object> in) {
    v8::Local<v8::Value> jsValue = check(in->Get(context, nameHandle.Get(isolate)));
    return wrapper.template unwrap<Type>(
        context, jsValue, TypeErrorContext::structField(typeid(Struct), exportedName), in);
  }

private:
  v8::Global<v8::Name> nameHandle;
};

template <typename... T>
struct TypeTuple {
  using Indexes = kj::_::MakeIndexes<sizeof...(T)>;
};

template <typename Self, typename T, typename FieldWrapperTuple,
          typename Indices = typename FieldWrapperTuple::Indexes>
class StructWrapper;

// TypeWrapper mixin for struct types (application-defined C++ structs declared with a
// JSG_STRUCT block).
template <typename Self, typename T, typename... FieldWrappers, size_t... indices>
class StructWrapper<Self, T, TypeTuple<FieldWrappers...>, kj::_::Indexes<indices...>> {
public:
  static const JsgKind JSG_KIND = JsgKind::STRUCT;

  static constexpr const std::type_info& getName(T*) { return typeid(T); }

  v8::Local<v8::Object> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, T&& in) {
    auto isolate = context->GetIsolate();
    v8::EscapableHandleScope handleScope(isolate);
    auto& fields = getFields(isolate);
    v8::Local<v8::Object> out = v8::Object::New(isolate);
    (kj::get<indices>(fields).wrap(
        static_cast<Self&>(*this), isolate, context, creator, in, out), ...);
    return handleScope.Escape(out);
  }

  kj::Maybe<T> tryUnwrap(
      v8::Local<v8::Context> context, v8::Local<v8::Value> handle, T*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // In the case that an individual field is the wrong type, we don't return null, but throw an
    // exception directly. This is because:
    // 1) If we returned null, we'd lose useful debugging information about which exact field was
    //    incorrectly typed.
    // 2) Returning null is intended to allow calling code to probe for different types, e.g. to
    //    allow a parameter which is "either a String or an ArrayBuffer". Such probing really
    //    intends to check the top-level type. Recursively probing all fields in order to check
    //    if they match probably isn't a practical use case, since it would be inefficient and
    //    could lead to ambiguous results, especially when fields are optional.
    //
    // For similar reasons, if we are initializing this dictionary from null/undefined, and the
    // dictionary has required members, we throw.

    auto isolate = context->GetIsolate();

    if (handle->IsUndefined() || handle->IsNull()) {
      if constexpr(((webidl::isOptional<typename FieldWrappers::Type> ||
                     kj::isSameType<typename FieldWrappers::Type, Unimplemented>()) && ...)) {
        return T{};
      }
      jsg::throwTypeError(isolate, "Cannot initialize a dictionary with required members from an "
                                    "undefined or null value.");
    }

    if (!handle->IsObject()) return kj::none;

    v8::HandleScope handleScope(isolate);
    auto& fields = getFields(isolate);
    auto in = handle.As<v8::Object>();

    // Note: We unwrap struct members in the order in which the compiler evaluates the expressions
    //   in `T { expressions... }`. This is technically a non-conformity from Web IDL's perspective:
    //   it prescribes lexicographically-ordered member initialization, with base members ordered
    //   before derived members. Objects with mutating getters might be broken by this, but it
    //   doesn't seem worth fixing absent a compelling use case.

    return T {
      kj::get<indices>(fields).unwrap(static_cast<Self&>(*this), isolate, context, in)...
    };
  }

  void newContext() = delete;
  void getTemplate() = delete;

private:
  kj::Maybe<kj::Tuple<FieldWrappers...>> lazyFields;

  kj::Tuple<FieldWrappers...>& getFields(v8::Isolate* isolate) {
    KJ_IF_SOME(f, lazyFields) {
      return f;
    } else {
      return lazyFields.emplace(kj::tuple(FieldWrappers(isolate)...));
    }
  }
};

}  // namespace workerd::jsg
