// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// Translates between C++ struct types and JavaScript objects. This translation is by value: the
// struct is translated to/from a native JS object with the same field names.

#include <workerd/jsg/util.h>
#include <workerd/jsg/value.h>
#include <workerd/jsg/web-idl.h>

#include <kj/vector.h>

#include <concepts>
#include <span>
#include <string_view>
#include <type_traits>

namespace workerd::jsg {

template <typename T>
constexpr bool isV8LocalOrData = isV8Local<T>() || std::is_base_of_v<v8::Data, T> || IsJsValue<T>;

template <typename T>
constexpr bool isV8LocalOrData<kj::Maybe<T>> = isV8LocalOrData<T>;

template <typename T>
constexpr bool isV8LocalOrData<Optional<T>> = isV8LocalOrData<T>;

template <typename T>
constexpr bool isV8LocalOrData<LenientOptional<T>> = isV8LocalOrData<T>;

template <typename T>
constexpr bool isV8LocalOrData<kj::Array<T>> = isV8LocalOrData<T>;

template <typename T>
constexpr bool isV8LocalOrData<kj::ArrayPtr<T>> = isV8LocalOrData<T>;

template <typename T>
constexpr bool isV8LocalOrData<Dict<T>> = isV8LocalOrData<T>;

template <typename T, typename... Rest>
constexpr bool isV8LocalOrData<kj::OneOf<T, Rest...>> =
    isV8LocalOrData<T> || (isV8LocalOrData<Rest> || ...);

// JSG_STRUCT member fields really should not be v8::Locals, v8::Datas, or JsValues because
// there's no guarantee the v8::HandleScope will be valid when the field is accessed. Instead
// they should be wrapped in jsg::V8Ref or jsg::JsRef. However, we only want to enforce this
// for JSG_STRUCTs that we *receive* from JS, not for JSG_STRUCTs that we *send* to JS, so
// we only actually apply this check when unwrapping (JS -> C++). Why? Great question! It's
// because when we are sending a struct to JS, we know we have a valid v8::HandleScope and
// it's fairly expensive to create a jsg::JsRef/jsg::V8Ref, especially when we need to do
// so repeatedly (e.g. for an iterator, for instance).
template <typename T>
concept NotV8Local = !isV8LocalOrData<T>;

// Just to be sure we got the concept right...
static_assert(NotV8Local<int>);
static_assert(NotV8Local<kj::String>);
static_assert(NotV8Local<kj::Array<int>>);
static_assert(NotV8Local<kj::Maybe<kj::String>>);
static_assert(NotV8Local<kj::OneOf<int, kj::String>>);
static_assert(!NotV8Local<kj::Maybe<v8::Local<v8::Object>>>);
static_assert(!NotV8Local<kj::Maybe<JsValue>>);
static_assert(!NotV8Local<jsg::Optional<v8::Local<v8::Object>>>);
static_assert(!NotV8Local<jsg::Optional<JsObject>>);
static_assert(!NotV8Local<kj::OneOf<int, v8::Local<v8::Object>>>);
static_assert(!NotV8Local<kj::OneOf<int, JsValue>>);
static_assert(!NotV8Local<kj::OneOf<int, kj::String, kj::Maybe<JsValue>>>);
static_assert(
    !NotV8Local<kj::OneOf<int, kj::String, kj::Maybe<kj::OneOf<int, kj::Maybe<JsValue>>>>>);
static_assert(!NotV8Local<v8::Local<v8::Object>>);
static_assert(!NotV8Local<JsValue>);
static_assert(!NotV8Local<v8::Local<v8::Value>>);
static_assert(!NotV8Local<v8::Value>);
static_assert(!NotV8Local<kj::Array<JsValue>>);
static_assert(!NotV8Local<kj::Array<v8::Local<v8::Object>>>);
static_assert(!NotV8Local<Dict<JsValue>>);

// Converts one field of a JSG_STRUCT to/from the corresponding JavaScript property. Note that the
// struct that declares the field, the field's location within that struct, and the field's JS-
// visible name are supplied by StructWrapper at the call site to avoid excessive templating.
template <typename TypeWrapper, typename T>
class FieldWrapper {
 public:
  using Type = T;

  FieldWrapper(v8::Isolate* isolate, kj::StringPtr exportedName)
      : exportedName(exportedName),
        nameHandle(isolate, v8StrIntern(isolate, exportedName)) {}

  // The is the original, slow-path wrap implementation that uses Set(). Prefer the other overload
  // for better performance. It is, however, a breaking change to remove this overload so we
  // need to keep it with a compatibility flag.
  void wrap(Lock& js,
      TypeWrapper& wrapper,
      v8::Isolate* isolate,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      T& value,
      v8::Local<v8::Object> out) {
    if constexpr (kj::isSameType<T, SelfRef>()) {
      // Ignore SelfRef when converting to JS.
    } else if constexpr (kj::isSameType<T, Unimplemented>() || kj::isSameType<T, WontImplement>()) {
      // Fields with these types are required NOT to be present, so don't try to convert them.
    } else {
      if constexpr (webidl::OptionalType<Type>) {
        // Don't even set optional fields that aren't present.
        if (value == kj::none) return;
      }
      auto handle = wrapper.wrap(js, context, creator, kj::mv(value));
      check(out->Set(context, nameHandle.Get(isolate), handle));
    }
  }

  void wrap(Lock& js,
      TypeWrapper& wrapper,
      v8::Isolate* isolate,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      T& value,
      v8::MaybeLocal<v8::Value>& out,
      size_t& idx) {
    if constexpr (kj::isSameType<T, SelfRef>()) {
      // Ignore SelfRef when converting to JS.
    } else if constexpr (kj::isSameType<T, Unimplemented>() || kj::isSameType<T, WontImplement>()) {
      // Fields with these types are required NOT to be present, so don't try to convert them.
    } else {
      idx++;
      out = wrapper.wrap(js, context, creator, kj::mv(value));
    }
  }

  // `structType` identifies the struct declaring this field and is used for type error messages.
  Type unwrap(TypeWrapper& wrapper,
      v8::Isolate* isolate,
      v8::Local<v8::Context> context,
      v8::Local<v8::Object> in,
      const std::type_info& structType) {
    static_assert(NotV8Local<Type>);
    auto& js = Lock::from(isolate);
    auto fieldName = nameHandle.Get(isolate);
    v8::Local<v8::Value> jsValue = v8::Undefined(isolate);
    if (!js.isJavascriptExecutionDisallowed()) {
      jsValue = check(in->Get(context, fieldName));
    } else {
      // Safe path to get a v8::Value under the `DisallowJavascriptExecution` scope without
      // walking the prototype chain, hence skipping any Object.prototype getters
      // NOTE: GetRealNamedProperty() can technically execute user-defined getters on the object
      // itself, but this path only deals with plain objects produced by ValueDeserializer
      // NOTE: We must check HasRealNamedProperty() first because GetRealNamedProperty()
      // returns an empty v8::Maybe both in-case of an error and in-case the property
      // does not exist
      if (check(in->HasRealNamedProperty(context, fieldName))) {
        jsValue = check(in->GetRealNamedProperty(context, fieldName));
      }
    }
    return wrapper.template unwrap<Type>(
        js, context, jsValue, TypeErrorContext::structField(structType, exportedName.cStr()), in);
  }

 private:
  kj::StringPtr exportedName;
  v8::Global<v8::Name> nameHandle;
};

// The fields of a JSG_STRUCT, as pointers to members of the struct. Generated by JSG_STRUCT.
template <auto... fields>
struct StructFields {
  using Indexes = kj::_::MakeIndexes<sizeof...(fields)>;
};

template <typename T>
struct FieldType_;
template <typename Struct, typename T>
struct FieldType_<T Struct::*> {
  using Type = T;
};

// The declared type of the struct field that `field` points to.
template <auto field>
using FieldType = typename FieldType_<decltype(field)>::Type;

template <typename Self, typename T, typename Fields, typename Indices = Fields::Indexes>
class StructWrapper;

// TypeWrapper mixin for struct types (application-defined C++ structs declared with a
// JSG_STRUCT block).
template <typename Self, typename T, auto... fields, size_t... indices>
class StructWrapper<Self, T, StructFields<fields...>, kj::_::Indexes<indices...>> {
 public:
  static const JsgKind JSG_KIND = JsgKind::STRUCT;

  static constexpr const std::type_info& getName(T*) {
    return typeid(T);
  }

  // A count of the JSG_STRUCT fields that are usable for the v8::DictionaryTemplate
  // version of wrap (i.e. not SelfRef, Unimplemented, or WontImplement).
  static constexpr size_t kCountOfUsableFields =
      ((isUsableStructField<FieldType<fields>> ? 1 : 0) + ...);

  v8::Local<v8::Object> wrap(
      Lock& js, v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, T&& in) {
    auto isolate = js.v8Isolate;
    auto& fieldWrappers = getFields(isolate);

    // Fast path using a cached dictionary template.
    if (js.isUsingFastJsgStruct()) {
      v8::MaybeLocal<v8::Value> values[kCountOfUsableFields]{};

      size_t idx = 0;
      (kj::get<indices>(fieldWrappers)
              .wrap(js, static_cast<Self&>(*this), isolate, context, creator, in.*fields,
                  values[idx], idx),
          ...);

      // We use a cached dictionary template to improve performance on repeated struct wraps.

      v8::Local<v8::DictionaryTemplate> tmpl;
      if (templateHandle.IsEmpty()) {
        tmpl = makeTemplate(isolate);
        templateHandle.Reset(isolate, tmpl);
      } else {
        tmpl = templateHandle.Get(isolate);
      }

      // Make sure we filled in the expected number of fields.
      KJ_ASSERT(idx == kCountOfUsableFields);

      return tmpl->NewInstance(context, values);
    }

    // Original slow path.
    v8::Local<v8::Object> out = v8::Object::New(isolate);
    (kj::get<indices>(fieldWrappers)
            .wrap(js, static_cast<Self&>(*this), isolate, context, creator, in.*fields, out),
        ...);
    return out;
  }

  kj::Maybe<T> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      T*,
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

    if (handle->IsUndefined() || handle->IsNull()) {
      if constexpr (((webidl::OptionalType<FieldType<fields>> ||
                         kj::isSameType<FieldType<fields>, Unimplemented>()) &&
                        ...)) {
        return T{};
      }
      jsg::throwTypeError(js.v8Isolate,
          kj::str("Cannot initialize ", typeid(T).name(),
              " with required members from an "
              "undefined or null value."));
    }

    if (!handle->IsObject()) return kj::none;

    auto& fieldWrappers = getFields(js.v8Isolate);
    auto in = handle.As<v8::Object>();

    // Note: We unwrap struct members in the order in which the compiler evaluates the expressions
    //   in `T { expressions... }`. This is technically a non-conformity from Web IDL's perspective:
    //   it prescribes lexicographically-ordered member initialization, with base members ordered
    //   before derived members. Objects with mutating getters might be broken by this, but it
    //   doesn't seem worth fixing absent a compelling use case.
    auto t = T{kj::get<indices>(fieldWrappers)
                   .unwrap(static_cast<Self&>(*this), js.v8Isolate, context, in, typeid(T))...};

    // Note that if a `validate` function is provided, then it will be called after the struct is
    // unwrapped from v8. This would be an appropriate time to throw an error.
    // Signature: void validate(jsg::Lock& js);
    if constexpr (requires { t.validate(js); }) {
      t.validate(js);
    }

    return t;
  }

  void newContext() = delete;
  void getTemplate() = delete;

 private:
  using FieldWrappers = kj::Tuple<FieldWrapper<Self, FieldType<fields>>...>;

  v8::Global<v8::DictionaryTemplate> templateHandle;
  kj::Maybe<FieldWrappers> lazyFields;

  FieldWrappers& getFields(v8::Isolate* isolate) {
    KJ_IF_SOME(f, lazyFields) {
      return f;
    } else {
      return lazyFields.emplace(
          kj::tuple(FieldWrapper<Self, FieldType<fields>>(isolate, T::jsgFieldNames[indices])...));
    }
  }

  // Builds the dictionary template used by the fast path of wrap(). It describes only the fields
  // that wrap() actually fills in, in the same order.
  static v8::Local<v8::DictionaryTemplate> makeTemplate(v8::Isolate* isolate) {
    static constexpr bool isUsable[] = {isUsableStructField<FieldType<fields>>...};
    kj::Vector<std::string_view> names(kCountOfUsableFields);
    for (auto i: kj::indices(T::jsgFieldNames)) {
      if (isUsable[i]) {
        names.add(std::string_view(T::jsgFieldNames[i].begin(), T::jsgFieldNames[i].size()));
      }
    }
    return v8::DictionaryTemplate::New(
        isolate, std::span<const std::string_view>(names.begin(), names.size()));
  }
};

}  // namespace workerd::jsg
