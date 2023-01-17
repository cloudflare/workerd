// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Main public interface to JSG library.
//
// Any files declaring an API to export to JavaScript will need to include this header.

#include <kj/string.h>
#include <kj/function.h>
#include <kj/exception.h>
#include <kj/one-of.h>
#include <type_traits>
#include <v8.h>
#include "macro-meta.h"
#include "wrappable.h"

#define JSG_ASSERT(cond, jsErrorType, ...)                                              \
  KJ_ASSERT(cond, kj::str(JSG_EXCEPTION(jsErrorType) ": ", ##__VA_ARGS__))

#define JSG_REQUIRE(cond, jsErrorType, ...)                                             \
  KJ_REQUIRE(cond, kj::str(JSG_EXCEPTION(jsErrorType) ": ", ##__VA_ARGS__))
// Unlike KJ_REQUIRE, JSG_REQUIRE passes all message arguments through kj::str which makes it
// "prettier". This does have some implications like if there's only string literal arguments then
// there's an unnecessary heap copy. More importantly none of the expressions you pass in end up in
// the resultant string AND you are responsible for formatting the resultant string. For example,
// KJ_REQUIRE(false, "some message", x) formats it like "some message; x = 5". The "equivalent" via
// this macro would be JSG_REQUIRE(false, "some message ", x); which would yield a string like
// "some message 5" (or JSG_REQUIRE(false, "some message; x = ", x) if you wanted identical output,
// but then why not use KJ_REQUIRE).

#define JSG_REQUIRE_NONNULL(value, jsErrorType, ...)                                    \
  KJ_REQUIRE_NONNULL(value, kj::str(JSG_EXCEPTION(jsErrorType) ": ", ##__VA_ARGS__))
// JSG_REQUIRE + KJ_REQUIRE_NONNULL.

#define JSG_FAIL_REQUIRE(jsErrorType, ...)                                              \
  KJ_FAIL_REQUIRE(kj::str(JSG_EXCEPTION(jsErrorType) ": ", ##__VA_ARGS__))
// JSG_REQUIRE + KJ_FAIL_REQUIRE

// These are passthrough functions to KJ. We expect the error string to be
// surfaced to the application.

#define _JSG_INTERNAL_REQUIRE(cond, jsErrorType, ...)                                             \
  do {                                                                                             \
    try {                                                                                          \
      KJ_REQUIRE(cond, jsErrorType ": Cloudflare internal error.");                                \
    } catch (const kj::Exception& e) {                                                             \
      KJ_LOG(ERROR, e, ##__VA_ARGS__);                                                             \
      throw e;                                                                                     \
    }                                                                                              \
  } while (0)

#define _JSG_INTERNAL_REQUIRE_NONNULL(value, jsErrorType, ...)                                    \
  ([&]() -> decltype(auto) {                                                                       \
    try {                                                                                          \
      return KJ_REQUIRE_NONNULL(value, jsErrorType ": Cloudflare internal error.");                \
    } catch (const kj::Exception& e) {                                                             \
      KJ_LOG(ERROR, e, ##__VA_ARGS__);                                                             \
      throw e;                                                                                     \
    }                                                                                              \
  }())

#define _JSG_INTERNAL_FAIL_REQUIRE(jsErrorType, ...)                                              \
  do {                                                                                             \
    try {                                                                                          \
      KJ_FAIL_REQUIRE(jsErrorType ": Cloudflare internal error.");                                 \
    } catch (const kj::Exception& e) {                                                             \
      KJ_LOG(ERROR, e, ##__VA_ARGS__);                                                             \
      throw e;                                                                                     \
    }                                                                                              \
  } while (0)

#define JSG_EXCEPTION(jsErrorType) JSG_ERROR_ ## jsErrorType
#define JSG_DOM_EXCEPTION(name) "jsg.DOMException(" name ")"
#define JSG_INTERNAL_DOM_EXCEPTION(name) "jsg-internal.DOMException(" name ")"

#define JSG_ERROR_DOMOperationError JSG_DOM_EXCEPTION("OperationError")
#define JSG_ERROR_DOMDataError JSG_DOM_EXCEPTION("DataError")
#define JSG_ERROR_DOMDataCloneError JSG_DOM_EXCEPTION("DataCloneError")
#define JSG_ERROR_DOMInvalidAccessError JSG_DOM_EXCEPTION("InvalidAccessError")
#define JSG_ERROR_DOMInvalidStateError JSG_DOM_EXCEPTION("InvalidStateError")
#define JSG_ERROR_DOMInvalidCharacterError JSG_DOM_EXCEPTION("InvalidCharacterError")
#define JSG_ERROR_DOMNotSupportedError JSG_DOM_EXCEPTION("NotSupportedError")
#define JSG_ERROR_DOMSyntaxError JSG_DOM_EXCEPTION("SyntaxError")
#define JSG_ERROR_DOMTimeoutError JSG_DOM_EXCEPTION("TimeoutError")
#define JSG_ERROR_DOMTypeMismatchError JSG_DOM_EXCEPTION("TypeMismatchError")
#define JSG_ERROR_DOMQuotaExceededError JSG_DOM_EXCEPTION("QuotaExceededError")
#define JSG_ERROR_DOMAbortError JSG_DOM_EXCEPTION("AbortError")

#define JSG_ERROR_TypeError "jsg.TypeError"
#define JSG_ERROR_Error "jsg.Error"
#define JSG_ERROR_RangeError "jsg.RangeError"

#define JSG_ERROR_InternalDOMOperationError JSG_INTERNAL_DOM_EXCEPTION("OperationError")

#define JSG_KJ_EXCEPTION(type, jsErrorType, ...)                                        \
  kj::Exception(kj::Exception::Type::type, __FILE__, __LINE__,                           \
                kj::str(JSG_EXCEPTION(jsErrorType) ": ", __VA_ARGS__))

namespace workerd::jsg {
  kj::String stringifyHandle(v8::Local<v8::Value> value);
}

namespace v8 {
  template <typename T, typename = kj::EnableIf<kj::canConvert<T*, v8::Value*>()>>
  kj::String KJ_STRINGIFY(v8::Local<T> value) {
    // Allows v8 handles to be passed to kj::str() as well as KJ_LOG and related macros.
    return workerd::jsg::stringifyHandle(value);
  }
}

namespace workerd::jsg {

using kj::byte;

class JsExceptionThrown: public std::exception {
public:
  JsExceptionThrown();
  const char* what() const noexcept override;

private:
  void* trace[16];
  size_t traceSize;
  mutable kj::String whatBuffer;
};
// When a C++ callback wishes to throw a JavaScript exception, it should first call
// isolate->ThrowException() to set the JavaScript error value, then it should throw
// JsExceptionThrown() as a C++ exception. This will be caught by the callback glue before the
// code returns to V8.
//
// This differs from the usual convention in V8 which is to return a v8::Maybe that is null in the
// case an exception is thrown. Writing code that deals with maybes is cumbersome and error-prone
// compared to C++ exceptions.

// =======================================================================================
// Macros for declaring type glue.

#define JSG_RESOURCE_TYPE(Type, ...) \
  static constexpr ::workerd::jsg::JsgKind JSG_KIND KJ_UNUSED = \
      ::workerd::jsg::JsgKind::RESOURCE; \
  using jsgSuper = jsgThis; \
  using jsgThis = Type; \
  template <typename> \
  friend constexpr bool ::workerd::jsg::resourceNeedsGcTracing(); \
  template <typename T> \
  friend void ::workerd::jsg::visitSubclassForGc( \
      T* obj, ::workerd::jsg::GcVisitor& visitor); \
  inline void jsgVisitForGc(::workerd::jsg::GcVisitor& visitor) override { \
    jsgSuper::jsgVisitForGc(visitor); \
    ::workerd::jsg::visitSubclassForGc<Type>(this, visitor); \
  } \
  static void jsgConfiguration(__VA_ARGS__); \
  template <typename Registry, typename Self> \
  static void registerMembers(Registry& registry, ##__VA_ARGS__)
// Begins a block nested inside a C++ class to declare how that class should be accessible in
// JavaScript. JSG_RESOURCE_TYPE declares that the class is a "resource type" in KJ parlance.
//
// https://github.com/sandstorm-io/capnproto/blob/master/style-guide.md#value-types-vs-resource-types
//
// In short, this means that the type is normally passed by reference, and that when JavaScript
// code accesses members of the type, it calls back into C++. This differs from value types, which
// are normally deep-copied into JavaScript objects such that C++ is no longer involved.
//
// Example usage:
//
//     class MyApiType: public jsg::Object {
//       // Some type we want to expose to JavaScript.
//     public:
//       static jsg::Ref<MyType> constructor(bool b, kj::String s);
//       // Called when JavaScript invokes `new MyType()`. The name `constructor` is special.
//       // If you do not declare a constructor, then attempts to construct the type from
//       // JavaScript will throw an exception, but you'll still be able to construct it in C++
//       // using the regular C++ constructor(s).
//
//       void foo(int i, kj::String str);
//       double bar();
//       // Methods that can be called from JavaScript.
//
//       kj::StringPtr getBaz();
//       void setBaz(kj::String value);
//       // Methods implementing a property.
//
//       JSG_RESOURCE_TYPE(MyApiType) {
//         JSG_METHOD(foo);
//         JSG_METHOD(bar);
//         JSG_INSTANCE_PROPERTY(baz, getBaz, setBaz);
//       }
//
//     private:
//       void visitForGc(jsg::GcVisitor visitor);
//       // If this type contains any Ref or Value objects, it must implement visitForGc(), and when
//       // this is called, it must call `visitor.visit()` on all handles that it knows about. If
//       // the object doesn't hold any JS handles then it need not implement this. See the
//       // definition of GcVisitor, below, for more information.
//
//       jsg::Value someValue;
//       jsg::Ref<MyOtherApiType> someOtherResourceObject;
//       jsg::V8Ref<v8::Map> someState;
//       // Objects of resource type may be destroyed outside of the isolate lock. Therefore, if you
//       // need to hold a reference to a V8 object in a resource type, you should use one of these
//       // classes / class templates. In particular, holding a raw v8::Global<T> may result in
//       // undefined behavior upon destruction.
//     };
//
// Notice that method parameters and return types are automatically converted between C++ and
// JavaScript. You specify the full set of types that your JavaScript execution environment will
// support when you declare your Isolate (usually in high-level code).
//
// Additionally, the following types are always supported:
// - C++ double, int <-> JS Number
// - C++ kj::Date <-> JS Date in return position, JS Date or millisecond unix epoch as argument
// - C++ kj::String, kj::StringPtr <-> JS String
// - C++ kj::Maybe<T> <-> JS null or T
// - C++ jsg::Optional<T> <-> JS undefined or T
// - C++ jsg::LenientOptional<T> <-> JS undefined or T (treats type errors as JS undefined)
// - C++ kj::OneOf<T, U, ...> <-> JS T or U or ...
// - C++ kj::Array<T> <-> JS Array of T
// - C++ kj::Array<byte> <-> JS ArrayBuffer
// - C++ jsg::Dict<T> <-> JS Object used as a map of strings to values of type T
// - C++ jsg::Function<T(U, V, ...)> <-> JS Function
// - C++ jsg::Promise<T> <-> JS Promise
// - C++ jsg::Ref<T> <-> JavaScript resource type
// - C++ v8::Local<T> <-> JavaScript value
//
// There is also some magic. If the first parameter to a method has type
// `const v8::FunctionCallbackInfo<v8::Value>&`, then it will receive the FunctionCallbackInfo
// as passed from V8. (For property accessors, this should be PropertyCallbackInfo instead.) This
// gives you an escape hatch by which you can directly access the V8 context when needed. In this
// case the second parameter to your method will correspond to the first parameter passed from
// JavaScript.
//
// As another piece of magic, you can add some special types to the end of your parameter list in
// order to receive functionality from the JavaScript environment itself. These parameters will not
// actually correspond to JavaScript parameters, and should always be placed at the end of the
// argument list. They are:
//
// - const jsg::TypeHandler<T>&: Provides callback which can be used to convert between V8 handles
//     and a C++ object of type T, and (for resource types) to allocate objects of type T on the V8
//     heap. The reference is valid only until your method returns.
// - `v8::Isolate*`: Receives the V8 isolate pointer.
//
// In yet more magic, you can add a single configuration parameter to the JSG_RESOURCE_TYPE macro:
//
//     class MyApiType: ... {
//     public:
//       JSG_RESOURCE_TYPE(MyApiType, uint apiVersion) {
//         if (apiVersion > 42) {
//           using namespace newapi;
//           JSG_NESTED_TYPE(Widget);
//         } else {
//           using namespace oldapi;
//           JSG_NESTED_TYPE(Widget);
//         }
//       }
//     };
//
// Populate the configuration parameter by passing it to the JSG isolate's constructor (the type
// declared by JSG_DECLARE_ISOLATE_TYPE in setup.h).
//
// Different resource types may have different configuration types. However, all the configuration
// types must be constructable from a single "meta" configuration type, which is the type of the
// configuration passed to the JSG isolate's constructor.

#define JSG_CALLABLE(name) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerCallable<NAME, decltype(&Self::name), &Self::name>(); \
  } while (false)
// Use inside a JSG_RESOURCE_TYPE to declare that the resource type itself can be invoked as
// a function.

#define JSG_CALLABLE_NAMED(name, method) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerCallable<NAME, decltype(&Self::method), &Self::method>(); \
  } while (false)
// Use inside a JSG_RESOURCE_TYPE to declare that the instance of the resource type itself can
// be invoked as a function.

#define JSG_METHOD(name) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerMethod<NAME, decltype(&Self::name), &Self::name>(); \
  } while (false)
// Use inside a JSG_RESOURCE_TYPE block to declare that the given method should be callable from
// JavaScript on instances of the resource type.

#define JSG_METHOD_NAMED(name, method) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerMethod<NAME, decltype(&Self::method), &Self::method>(); \
  } while (false)
// Like JSG_METHOD but allows you to specify a different name to use in JavaScript. This is
// particularly useful when a JavaScript API wants to use a name that is a keyword in C++. For
// example:
//
//     JSG_METHOD_NAMED(delete, delete_);

#define JSG_STATIC_METHOD(name) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerStaticMethod<NAME, decltype(Self::name), &Self::name>(); \
  } while (false)
// Use inside a JSG_RESOURCE_TYPE block to declare that the given method should be callable from
// JavaScript on the resource type's constructor.

#define JSG_STATIC_METHOD_NAMED(name, method) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerStaticMethod<NAME, decltype(Self::method), &Self::method>(); \
  } while (false)
// Like JSG_METHOD_NAMED, but for static methods.

#define JSG_ITERABLE(method) \
  do { \
    static const char NAME[] = #method; \
    registry.template registerIterable<NAME, decltype(&Self::method), &Self::method>(); \
  } while (false)
// Use inside a JSG_RESOURCE_TYPE block to make objects of this type iterable. Pass in the name of
// a method returning an object satisfying the requirements of a JavaScript iterator. Note that this
// will NOT automatically register the method for you -- you still need to use JSG_METHOD{,_NAMED}
// if you plan to expose the method to JavaScript. For example:
//
//     struct Iterable {
//       static Iterable constructor();
//       Iterator entries();
//       JSG_RESOURCE_TYPE {
//         JSG_ITERABLE(entries);
//       }
//     };
//
// will allow a resource type to be iterated over, but not its entries() function to be called.
//
//     for (let x of new Iterable()) { /* ... */ }            // GOOD
//     for (let x of new Iterable().entries()) { /* ... */ }  // BAD!
//
// To enable the latter case, you would need to use JSG_METHOD(entries), and make Iterator itself
// iterable.

#define JSG_ASYNC_ITERABLE(method) \
  do { \
    static const char NAME[] = #method; \
    registry.template registerAsyncIterable<NAME, decltype(&Self::method), &Self::method>(); \
  } while (false)
// Use inside a JSG_RESOURCE_TYPE block to make objects of this type async iterable. Pass in the
// name of a method returning a kj::Promise for an object satisfying the requirements of a
// JavaScript iterator.

#define JSG_INSTANCE_PROPERTY(name, getter, setter) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerInstanceProperty<NAME, decltype(&Self::getter), &Self::getter, \
        decltype(&Self::setter), &Self::setter>(); \
  } while (false)
// Use inside a JSG_RESOURCE_TYPE block to declare a property on this object that should be
// accessible to JavaScript. `name` is the JavaScript member name, while `getter` and `setter` are
// the names of C++ methods that get and set this property.
//
// WARNING: This is usually not what you want. Usually you want JSG_PROTOTYPE_PROPERTY instead.
// Note that V8 implements instance properties by modifying the instance immediately after
// construction, which is inefficient and can break some optimizations. For example, any object
// with an instance proprety will not be possible to collect during minor GCs, only major GCs.
// Prototype properties are on the prototype, so have no runtime overhead until they are used.

#define JSG_PROTOTYPE_PROPERTY(name, getter, setter) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerPrototypeProperty<NAME, decltype(&Self::getter), &Self::getter, \
        decltype(&Self::setter), &Self::setter>(); \
  } while (false)
// Use inside a JSG_RESOURCE_TYPE block to declare a property on this object's prototype that
// should be accessible to JavaScript. `name` is the JavaScript member name, while `getter` and
// `setter` are the names of C++ methods that get and set this property.
//
// The key difference between JSG_INSTANCE_PROPERTY and JSG_PROTOTYPE_PROPERTY is in exactly how
// the getters and setters are attached to created JavaScript object. Specifically,
// JSG_INSTANCE_PROPERTY is similar to:
//
//   class Foo1 {
//     constructor() {
//       Object.defineProperty(this, 'bar', {
//         get() { /* ... */ },
//         set(v) { /* ... */ },
//       });
//     }
//   }
//
// Whereas JSG_PROTOTYPE_PROPERTY is equivalent to:
//
//   class Foo2 {
//     get bar() { /* ... */ }
//     set bar(v) { /* ... */ }
//   }
//
// The difference here is important because, in the former case, the properties are
// defined directly on *instances* of Foo1 as own properties, while in latter case,
// the properties are defined on the prototype of all Foo2 instances. In the former
// case, using JSG_INSTANCE_PROPERTY, the properties are directly enumerable on all instances
// of Foo1 such that calling Object.keys(new Foo1()) returns ['bar']. However, calling
// Object.keys(new Foo2()) will return an empty array [] as prototype properties are
// not directly enumerable.
//
// However, that's not the only critical difference. Because instance properties take
// precedence over prototype properties, Foo1 is not properly subclassable. If I did:
//
//   class MyFoo1 extends Foo1 {
//     get bar() { /** .. **/ }
//   }
//
//   const myFoo1 = new MyFoo1();
//   console.log(myFoo1.bar);
//
// The getter specified in the constructor of Foo1 would be called rather than the
// getter defined in the MyFoo1 class, which is not what a user would expect!
// This means that any resource type that uses JSG_INSTANCE_PROPERTY to attach properties
// will not be properly subclassable. To allow subclasses to work correctly, use
// JSG_PROTOTYPE_PROPERTY instead.

#define JSG_READONLY_INSTANCE_PROPERTY(name, getter) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerReadonlyInstanceProperty<NAME, \
        decltype(&Self::getter), &Self::getter>(); \
  } while (false)
// Like JSG_INSTANCE_PROPERTY but creates a property that will throw an exception if
// JavaScript tries to assign to it.

#define JSG_READONLY_PROTOTYPE_PROPERTY(name, getter) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerReadonlyPrototypeProperty< \
        NAME, decltype(&Self::getter), &Self::getter>(); \
  } while (false)
// Like JSG_PROTOTYPE_PROPERTY but creates a property that will throw an exception if JavaScript
// tries to assign to it.

#define JSG_LAZY_INSTANCE_PROPERTY(name, getter) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerLazyInstanceProperty< \
        NAME, decltype(&Self::getter), &Self::getter, false>(); \
  } while (false)
// A lazy property will call the getter the first time the property is access but will then
// replace the property definition with a normal instance property using the returned value.
// Keep in mind that, as an instance property, these lazily set properties cannot be overridden
// by subclasses. They are set directly on the instance object itself when it is created.

#define JSG_LAZY_READONLY_INSTANCE_PROPERTY(name, getter) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerLazyInstanceProperty< \
        NAME, decltype(&Self::getter), &Self::getter, true>(); \
  } while (false)

#define JSG_STATIC_CONSTANT(name) \
  do { \
    static const char NAME[] = #name; \
    registry.template registerStaticConstant<NAME, decltype(Self::name)>(Self::name); \
  } while (false)
// Use inside a JSG_RESOURCE_TYPE to create a static constant member on the constructor and
// prototype of this object. Only primitive data types (booleans, strings, numbers) are allowed.
// Unlike the JSG_INSTANCE_PROPERTY and JSG_READONLY_PROPERTY macros, this does not use a getter
// -- it expects a static constexpr member of a primitive type available in the class by the same
// name. For example:
//
//     struct Interface {
//       static Interface constructor()
//       static constexpr int FOO_BAR = 123;
//       JSG_RESOURCE_TYPE {
//         JSG_STATIC_CONSTANT(FOO_BAR);
//       }
//     };
//
// will allow all of the following JS expressions to hold true:
//
//     Interface.FOO_BAR                              === 123
//     Interface.prototype.FOO_BAR                    === 123
//     new Interface().FOO_BAR                        === 123
//     Object.getPrototypeOf(new Interface()).FOO_BAR === 123
//
// This is useful to implement constant interface members as specified in Web IDL:
//   https://heycam.github.io/webidl/#idl-constants
//
// TODO(someday): This should probably also support the null JS value.

#define JSG_INHERIT(Type) \
  static_assert(kj::canConvert<Self&, Type&>(), #Type " is not a superclass of this"); \
  registry.template registerInherit<Type>()

// Use inside a JSG_RESOURCE_TYPE block to declare that this type inherits from another type,
// which must also have a JSG_RESOURCE_TYPE block. This type must singly, non-virtually inherit
// from the specified type. (Multiple inheritance and virtual inheritance will not work since we
// rely on the pointer to the superclass and the subclass having the same numeric value.)

#define JSG_INHERIT_INTRINSIC(intrinsic) \
  do { \
      static const char NAME[] = #intrinsic; \
      registry.template registerInheritIntrinsic<NAME>(intrinsic); \
  } while (false)
// Use inside a JSG_RESOURCE_TYPE block to declare that this type inherits from an intrinsic
// prototype. This is primarily useful to inherit from v8::kErrorPrototype, like DOMException, and
// v8::kIteratorPrototype.

template <typename T, typename U>
using HasGetTemplateOverload = decltype(
    kj::instance<T&>().getTemplate((v8::Isolate*)nullptr, (U*)nullptr));
// An isDetected() operation which detects if the expression t.getTemplate(isolate, &u) is available
// for instances `t`, `u` of types T and U.

#define JSG_NESTED_TYPE(Type) \
  do { \
    static_assert(Type::JSG_KIND == ::workerd::jsg::JsgKind::RESOURCE, \
        #Type " is not a resource type, and therefore cannot not be declared nested"); \
    static const char NAME[] = #Type; \
    registry.template registerNestedType<Type, NAME>(); \
  } while (false)
// Use inside a JSG_RESOURCE_TYPE block to declare that the given type should be visible as a
// static member of this type. Typically, your "global" type would use several of these
// declarations to make other types appear in the global scope. It is not necessary for the types
// to be nested in C++.

#define JSG_NESTED_TYPE_NAMED(Type, Name) \
  do { \
    static_assert(Type::JSG_KIND == ::workerd::jsg::JsgKind::RESOURCE, \
        #Type " is not a resource type, and therefore cannot not be declared nested"); \
    static const char NAME[] = #Name; \
    registry.template registerNestedType<Type, NAME>(); \
  } while (false)

#define JSG_REFLECTION(...) \
  static constexpr bool jsgHasReflection = true; \
  template <typename TypeWrapper> \
  void jsgInitReflection(TypeWrapper& wrapper) { \
    jsgSuper::jsgInitReflection(wrapper); \
    wrapper.initReflection(this, __VA_ARGS__); \
  }
// Adds reflection to a resource type. See PropertyReflection<T> for usage.

#define JSG_TS_ROOT() \
  registry.registerTypeScriptRoot()
// Use inside a JSG_RESOURCE_TYPE block to declare that this type should be considered a "root" for
// the purposes of automatically generating TypeScript definitions. All "root" types and their
// recursively referenced types (e.g. method parameter/return types, property types, inherits, etc)
// will be included in the generated TypeScript. See the `## TypeScript` section of the JSG README.md
// for more details.

#define JSG_TS_OVERRIDE(...) \
 do { \
  static const char OVERRIDE[] = JSG_STRING_LITERAL(__VA_ARGS__); \
  registry.template registerTypeScriptOverride<OVERRIDE>(); \
 } while (false)
// Use inside a JSG_RESOURCE_TYPE block to customise the generated TypeScript definition for this type.
// This macro accepts a single override parameter containing a partial TypeScript statement definition.
// Varargs are accepted so that overrides can contain `,` outside of balanced brackets. See the
// `## TypeScript` section of the JSG README.md for many more details and examples.

#define JSG_TS_DEFINE(...) \
 do { \
  static const char DEFINE[] = JSG_STRING_LITERAL(__VA_ARGS__); \
  registry.template registerTypeScriptDefine<DEFINE>(); \
 } while (false)
// Use inside a JSG_RESOURCE_TYPE block to insert additional TypeScript definitions next to the generated
// TypeScript definition for this type. This macro accepts a single define parameter containing one or
// more TypeScript definitions (e.g. interfaces, classes, type aliases, consts, ...). Varargs are accepted
// so that defines can contain `,` outside of balanced brackets. See the `## TypeScript`section of the JSG
// README.md for more details.

#define JSG_STRUCT_TS_ROOT() \
  static constexpr bool _JSG_STRUCT_TS_ROOT_DO_NOT_USE_DIRECTLY = true
// Like JSG_TS_ROOT but for use with JSG_STRUCT. Should be placed adjacent to the JSG_STRUCT declaration,
// inside the same `struct` definition. See the `## TypeScript` section of the JSG README.md for more
// details.

#define JSG_STRUCT_TS_OVERRIDE(...) \
  static constexpr char _JSG_STRUCT_TS_OVERRIDE_DO_NOT_USE_DIRECTLY[] = JSG_STRING_LITERAL(__VA_ARGS__)
// Like JSG_TS_OVERRIDE but for use with JSG_STRUCT. Should be placed adjacent to the JSG_STRUCT
// declaration, inside the same `struct` definition. See the `## TypeScript` section of the JSG README.md
// for many more details and examples.

#define JSG_STRUCT_TS_DEFINE(...) \
  static constexpr char _JSG_STRUCT_TS_DEFINE_DO_NOT_USE_DIRECTLY[] = JSG_STRING_LITERAL(__VA_ARGS__)
// Like JSG_TS_DEFINE but for use with JSG_STRUCT. Should be placed adjacent to the JSG_STRUCT
// declaration, inside the same `struct` definition. See the `## TypeScript`section of the JSG README.md
// for more details.

namespace {
  template <typename T, typename = int>
  struct HasStructTypeScriptRoot : std::false_type {};
  template <typename T>
  struct HasStructTypeScriptRoot<T, decltype(T::_JSG_STRUCT_TS_ROOT_DO_NOT_USE_DIRECTLY, 0)> : std::true_type { };
  // true when the T has _JSG_STRUCT_TS_ROOT_DO_NOT_USE_DIRECTLY field generated by JSG_STRUCT_TS_ROOT

  template <typename T, typename = int>
  struct HasStructTypeScriptOverride : std::false_type {};
  template <typename T>
  struct HasStructTypeScriptOverride<T, decltype(T::_JSG_STRUCT_TS_OVERRIDE_DO_NOT_USE_DIRECTLY, 0)> : std::true_type { };
  // true when the T has _JSG_STRUCT_TS_OVERRIDE_DO_NOT_USE_DIRECTLY field generated by JSG_STRUCT_TS_OVERRIDE

  template <typename T, typename = int>
  struct HasStructTypeScriptDefine : std::false_type {};
  template <typename T>
  struct HasStructTypeScriptDefine<T, decltype(T::_JSG_STRUCT_TS_DEFINE_DO_NOT_USE_DIRECTLY, 0)> : std::true_type { };
  // true when the T has _JSG_STRUCT_TS_DEFINE_DO_NOT_USE_DIRECTLY field generated by JSG_STRUCT_TS_DEFINE
}

#define JSG_STRUCT(...) \
  static constexpr ::workerd::jsg::JsgKind JSG_KIND KJ_UNUSED = \
      ::workerd::jsg::JsgKind::STRUCT; \
  static constexpr char JSG_FOR_EACH(JSG_STRUCT_FIELD_NAME, , __VA_ARGS__); \
  template <typename Registry, typename Self> \
  static void registerMembers(Registry& registry) { \
    JSG_FOR_EACH(JSG_STRUCT_REGISTER_MEMBER, , __VA_ARGS__); \
    if constexpr (::workerd::jsg::HasStructTypeScriptRoot<Self>::value) { \
      registry.registerTypeScriptRoot(); \
    } \
    if constexpr (::workerd::jsg::HasStructTypeScriptOverride<Self>::value) { \
      registry.template registerTypeScriptOverride<Self::_JSG_STRUCT_TS_OVERRIDE_DO_NOT_USE_DIRECTLY>(); \
    } \
    if constexpr (::workerd::jsg::HasStructTypeScriptDefine<Self>::value) { \
      registry.template registerTypeScriptDefine<Self::_JSG_STRUCT_TS_DEFINE_DO_NOT_USE_DIRECTLY>(); \
    } \
  } \
  template <typename TypeWrapper, typename Self> \
  using JsgFieldWrappers = ::workerd::jsg::TypeTuple< \
    JSG_FOR_EACH(JSG_STRUCT_FIELD, , __VA_ARGS__) \
  >
// Nest this inside a simple struct declaration in order to support translating it to/from a
// JavaScript object / Web IDL dictionary.
//
//   struct MyStruct {
//     double foo;
//     kj::String bar;
//     kj::String $public;
//
//     JSG_STRUCT(foo, bar, $public);
//   };
//
// All of the types which are supported as method parameter / return types are also supported as
// struct field types.
//
// Note that if you use `jsg::Optional<T>` as a field type, then the field will not be present at
// all in JavaScript when the optional is null in C++ (as opposed to the field being present but
// assigned the value `undefined`).
//
// Fields with a starting '$' will have that dollar sign prefix stripped in the JS binding. A
// motivating example to enact that change was WebCrypto which has a field in a dictionary called
// "public". '$' was chosen as a token we can use because it's a character for a C++ identifier. If
// the Javascript field needs to contain '$' for some reason (which they probably shouldn't since
// identifiers starting with $ are rare in JS land, especially for things the runtime would be
// exporting), then you should be able to use '$$' as the identifier prefix in C++ since only the
// first '$' gets stripped.

namespace {
template <size_t N>
consteval size_t prefixLengthToStrip(const char (&s)[N]) {
  return s[0] == '$' ? 1 : 0;
}
}

#define JSG_STRUCT_FIELD_NAME(_, name) name##_JSG_NAME_DO_NOT_USE_DIRECTLY[] = #name
// This string may not be what's actually exported to v8. For example, if it starts with a `$`, then
// this value will still contain the `$` even though the `FieldWrapper` template argument will have
// it stripped.
#define JSG_STRUCT_FIELD(_, name) ::workerd::jsg::FieldWrapper< \
  TypeWrapper, Self, decltype(::kj::instance<Self>().name), &Self::name, \
  name##_JSG_NAME_DO_NOT_USE_DIRECTLY, ::workerd::jsg::prefixLengthToStrip(#name)>
// (Internal implementation details for JSG_STRUCT.)
#define JSG_STRUCT_REGISTER_MEMBER(_, name) registry.template registerStructProperty< \
  name##_JSG_NAME_DO_NOT_USE_DIRECTLY, \
  decltype(::kj::instance<Self>().name), &Self::name>()
// (Internal implementation details for JSG_STRUCT.)

// =======================================================================================
// Special types
//
// These types can be used in C++ to represent various JavaScript idioms / Web IDL types.

class Lock;

class Data {
  // Arbitrary V8 data, wrapped for storage from C++. You can't do much with it, so instead you
  // should probably use V8Ref<T>, a version of this that's strongly typed.
  //
  // When storing a Value inside a C++ object that is itself exported back to JavaScript, make sure
  // to implement GC visitation -- see GcVisitor, below.
  //
  // It is safe to destroy a strong jsg::Data object outside of the isolate lock. In this case,
  // the underlying V8 handles will be added to a queue, to be destroyed the next time a thread
  // locks the isolate. This means their destruction is non-deterministic, but that is true of V8
  // objects anyway, due to the GC. Weak jsg::Data (i.e., those which are reachable by V8's GC,
  // see GcVisitor below) must still be destroyed under the isolate lock to guard against concurrent
  // modification with the GC.
  //
  // Move construction and move assignment of strong jsg::Data is well-defined even without
  // holding the isolate lock. That is, it is safe to move Values unless you have implemented GC
  // visitation for them. Moving jsg::Data which are reachable via GC vistation is undefined
  // behavior outside of an isolate lock.

public:
  Data(decltype(nullptr)) {}
  ~Data() noexcept(false) {
    destroy();
  }
  Data(Data&& other): isolate(other.isolate), handle(kj::mv(other.handle)) {
    KJ_IF_MAYBE(t, other.tracedHandle) {
      // `other` is a traced `Data`, but once moved, we don't assume the new location is traced.
      // So, we need to make the handle strong.
      handle.ClearWeak();

      // Presumably, `other` is about to be destroyed. The destructor of `TracedReference`, though,
      // does nothing, because it doesn't know if the reference is even still valid, since it
      // could be called during GC sweep time. But here, we know that `other` is definitely still
      // valid, because we wouldn't be moving from an unreachable object. So we should Reset() the
      // `TracedReference` so that V8 knows it's gone, which might make minor GCs more effective.
      t->Reset();

      other.tracedHandle = nullptr;
    }
    other.isolate = nullptr;
    assertInvariant();
    other.assertInvariant();
  }
  Data& operator=(Data&& other) {
    if (this != &other) {
      destroy();
      isolate = other.isolate;
      handle = kj::mv(other.handle);
      other.isolate = nullptr;
      KJ_IF_MAYBE(t, other.tracedHandle) {
        handle.ClearWeak();
        t->Reset();
        other.tracedHandle = nullptr;
      }
    }
    assertInvariant();
    other.assertInvariant();
    return *this;
  }
  KJ_DISALLOW_COPY(Data);

  Data(v8::Isolate* isolate, v8::Local<v8::Data> handle)
      : isolate(isolate), handle(isolate, handle) {}
  v8::Local<v8::Data> getHandle(v8::Isolate* isolate) { return handle.Get(isolate); }
  v8::Local<v8::Data> getHandle(Lock& js);
  // Interact with raw V8 types.

  Data addRef(v8::Isolate* isolate) { return Data(isolate, getHandle(isolate)); }
  Data addRef(Lock& js);

  inline bool operator==(const Data& other) const {
    return handle == other.handle;
  }

private:
  v8::Isolate* isolate = nullptr;
  // The isolate with which the handles below are associated.

  v8::Global<v8::Data> handle;
  // Handle to the value which will be marked strong if any untraced C++ references exist, weak
  // otherwise.

  kj::Maybe<v8::TracedReference<v8::Data>> tracedHandle;
  // When `handle` is weak, `tracedHandle` is a copy of it used to integrate with V8 GC tracing.
  // When `handle` is strong, we null out `tracedHandle`, because we don't need it, and it is
  // illegal to hold onto a traced handle without actually marking it during each trace.

  friend class GcVisitor;

  void destroy();

  // Debugging helpers.
  void assertInvariant() {
#ifdef KJ_DEBUG
    assertInvariantImpl();
#endif
  }
#ifdef KJ_DEBUG
  void assertInvariantImpl();
#endif
};

template <typename T>
class V8Ref: private Data {
  // A drop-in replacement for v8::Global<T>. Its big feature is that, like jsg::Data, a
  // jsg::V8Ref<T> is safe to destroy outside of the isolate lock.
  //
  // Generally you should prefer using jsg::Value (for v8::Value) or jsg::Ref<T>. Use a
  // jsg::V8Ref<T> when you need the type-safety of holding a handle to a specific V8 type.

public:
  V8Ref(decltype(nullptr)): Data(nullptr) {}
  V8Ref(v8::Isolate* isolate, v8::Local<T> handle): Data(isolate, handle) {}
  V8Ref(V8Ref&& other): Data(kj::mv(other)) {}
  V8Ref& operator=(V8Ref&& other) {
    Data::operator=(kj::mv(other));
    return *this;
  }
  KJ_DISALLOW_COPY(V8Ref);

  v8::Local<T> getHandle(v8::Isolate* isolate) {
    if constexpr (std::is_base_of<v8::Value, T>()) {
      // V8 doesn't let us cast directly from v8::Data to subtypes of v8::Value, so we're forced to
      // use this double cast... Ech.
      return Data::getHandle(isolate).template As<v8::Value>().template As<T>();
    } else {
      return Data::getHandle(isolate).template As<T>();
    }
  }
  v8::Local<T> getHandle(jsg::Lock& js);

  V8Ref addRef(v8::Isolate* isolate) { return V8Ref(isolate, getHandle(isolate)); }
  V8Ref addRef(jsg::Lock& js);

  V8Ref deepClone(jsg::Lock& js);

  inline bool operator==(const V8Ref& other) const {
    return Data::operator==(other);
  }

private:
  friend class GcVisitor;
};

using Value = V8Ref<v8::Value>;

template <typename T>
class HashableV8Ref: public V8Ref<T> {
  // Like V8Ref but also implements `hashCode()`. Useful as a key into a kj::HashTable.
  //
  // T must v8::Object or a subclass (or anything that implements GetIdentityHash()).

public:
  HashableV8Ref(decltype(nullptr)): V8Ref<T>(nullptr), identityHash(0) {}
  HashableV8Ref(v8::Isolate* isolate, v8::Local<T> handle)
      : V8Ref<T>(isolate, handle), identityHash(handle->GetIdentityHash()) {}
  HashableV8Ref(HashableV8Ref&& other) = default;
  HashableV8Ref& operator=(HashableV8Ref&& other) = default;
  KJ_DISALLOW_COPY(HashableV8Ref);

  HashableV8Ref addRef(v8::Isolate* isolate) {
    return HashableV8Ref(isolate, this->getHandle(isolate), identityHash);
  }
  HashableV8Ref addRef(jsg::Lock& js);

  int hashCode() const { return identityHash; }

private:
  int identityHash;

  HashableV8Ref(v8::Isolate* isolate, v8::Local<T> handle, int identityHash)
      : V8Ref<T>(isolate, handle), identityHash(identityHash) {}
};

template <typename T>
class Optional: public kj::Maybe<T> {
  // A value of type T, or `undefined`.
  //
  // In C++, this has the same usage as kj::Maybe<T>. However, a null kj::Maybe<T> corresponds to
  // `null` in JavaScript, whereas a null Optional<T> corresponds to `undefined` in JavaScript.
  //
  // Note: Due to Web IDL's undefined-to-nullable coercion rule, a null Maybe<T> can also unwrap
  //   from an `undefined` value explicitly passed to a non-optional nullable.
  //
  // There are two main use cases for Optional<T>: optional function/method parameters and optional
  // JSG_STRUCT members. In both cases, a null value in C++ corresponds to the parameter/field not
  // being present at all in JavaScript, or explicitly set to `undefined`.
  //
  // In Web IDL, function parameters are considered required unless marked `optional`, while
  // dictionary (JSG_STRUCT) members are considered optional unless marked `required`. So, if you
  // were implementing an API specified in Web IDL like so:
  //
  //     dictionary Data {
  //       double number;
  //       required DOMString string;
  //     };
  //     void foo(optional Data data);
  //
  // An appropriate representation in C++ would be:
  //
  //     struct Data {
  //       Optional<double> number;
  //       kj::String string;
  //       JSG_STRUCT(number, string);
  //     };
  //     void foo(Optional<Data> data);

public:
  template <typename... Params>
  Optional(Params&&... params): kj::Maybe<T>(kj::fwd<Params>(params)...) {}
  // Inheriting constructors does not inherit copy/move constructors, so we declare a forwarding
  // constructor instead.
};

template <typename T>
class LenientOptional: public kj::Maybe<T> {
  //  Identical to Optional, but rather than treating failures to unwrap a JS value to type T as an
  //  error, it just results in an unset LenientOptional.

public:
  template <typename... Params>
  LenientOptional(Params&&... params): kj::Maybe<T>(kj::fwd<Params>(params)...) {}
  // Inheriting constructors does not inherit copy/move constructors, so we declare a forwarding
  // constructor instead.
};

class SelfRef: public V8Ref<v8::Object> {
  // Use this type in a JSG_STRUCT to define a special field that will be filled in with a
  // reference to the original struct's JavaScript representation. This is useful e.g. if you
  // may need to pull additional fields out of the struct.
  //
  // Another option is to use jsg::Identified<MyStruct>, but sometimes storing the reference
  // into a field of the unwrapped struct is more convenient.
public:
  using V8Ref::V8Ref;
};

class ByteString: public kj::String {
  // TODO(cleanup): This class was meant to be a ByteString (characters in the range [0,255]), but
  //   its only use so far is in api::Headers. But making the Headers class use ByteStrings turned
  //   out to be unwise. Nevertheless, it is still useful to keep around in order to provide
  //   feedback to script authors when they are using header strings that may be incompatible with
  //   browser implementations of the Fetch spec.
  //
  //   Move this class to the `api` directory and rename to HeaderString.

public:
  template <typename... Params>
  explicit ByteString(Params&&... params): kj::String(kj::fwd<Params>(params)...) {}
  // Inheriting constructors does not inherit copy/move constructors, so we declare a forwarding
  // constructor instead.

  enum class Warning {
    NONE,                     // Contains 7-bit code points -- semantics won't change
    CONTAINS_EXTENDED_ASCII,  // Contains 8-bit code points -- semantics WILL change
    CONTAINS_UNICODE,         // Contains 16-bit code points -- semantics WILL change
  };
  Warning warning = Warning::NONE;
  // HACK: ByteString behaves just like a kj::String, but has this crappy enum to tell the code that
  //   consumes it that it contains a value which a real Web IDL ByteString would have encoded
  //   differently. We can't usefully do anything about the information in JSG, because we don't
  //   have access to the IoContext to print a warning in the inspector.
  //
  //   We default the enum to NONE so that ByteString(kj::str(otherHeader)) works as expected.
};

template <typename Value, typename Key = kj::String>
struct Dict {
  // A Dict<V, K> in C++ corresponds to a JavaScript object that is being used as a string -> value
  // map, where all the values are of type T.
  //
  // Note: A Dict<V, K> corresponds to a record<K, V> in the Web IDL language.
  //
  // TODO(someday): Maybe make this a map and not an array? Current use case doesn't care, though.

  struct Field {
    // Field of an object.

    Key name;
    Value value;
  };

  kj::Array<Field> fields;
};

template <typename T> class TypeHandler;

class Varargs {
  // An array of local values placed on the end of a parameter list to capture all trailing values
public:
  Varargs(size_t index, const v8::FunctionCallbackInfo<v8::Value>& args): startIndex(index), args(args) {
    if (index > args.Length()) {
      this->length = 0;
    } else {
      this->length = args.Length() - index;
    }
  }
  Varargs(Varargs&&) = default;
  KJ_DISALLOW_COPY(Varargs);

  size_t size() { return length; }

  Value operator[](size_t index) {
    return Value(args.GetIsolate(), args[startIndex + index]);
  }

  class Iterator {
    // TODO(cleanup): This is similar to capnp::_::IndexingIterator. Maybe a common utility class should be added to KJ.
  public:
    inline Iterator(size_t index, const v8::FunctionCallbackInfo<v8::Value>& args): index(index), args(args) {}

    inline Value     operator* () const { return Value(args.GetIsolate(), args[index]); }
    inline Iterator& operator++() { ++index; return *this; }
    inline Iterator  operator++(int) { return Iterator(index++, args); }
    inline ptrdiff_t operator- (const Iterator& other) const { return index - other.index; }

    inline bool operator==(const Iterator& other) const { return index == other.index && &args == &other.args; }
    inline bool operator!=(const Iterator& other) const { return index != other.index || &args != &other.args; }

  private:
    size_t index;
    const v8::FunctionCallbackInfo<v8::Value>& args;
  };

  Iterator begin() { return Iterator(startIndex, args); }
  Iterator end() { return Iterator(startIndex + length, args); };

private:
  size_t startIndex;
  size_t length;
  const v8::FunctionCallbackInfo<v8::Value>& args;
};

template <typename T>
constexpr bool resourceNeedsGcTracing();
template <typename T>
void visitSubclassForGc(T* obj, GcVisitor& visitor);

class Object: private Wrappable {
  // All resource types must inherit from this.

public:
  using jsgThis = Object;

  // Objects that extend from jsg::Object should never be copied or moved
  // independently of their owning jsg::Ref so we explicitly delete the
  // copy and move constructors and assignment operators to be safe.
  KJ_DISALLOW_COPY_AND_MOVE(Object);

  // Since we explicitly delete the copy and move constructors, we have
  // to explicitly declare the default constructor.
  Object() = default;

  inline void jsgVisitForGc(GcVisitor& visitor) override {}

  static constexpr bool jsgHasReflection = false;
  template <typename TypeWrapper>
  inline void jsgInitReflection(TypeWrapper& wrapper) {}

private:
  inline void visitForGc(GcVisitor& visitor) {}
  template <typename>
  friend constexpr bool ::workerd::jsg::resourceNeedsGcTracing();
  template <typename T>
  friend void visitSubclassForGc(T* obj, GcVisitor& visitor);
  template <typename T>
  friend class Ref;
  friend class kj::Refcounted;
  template <typename T>
  friend kj::Own<T> kj::addRef(T& object);
  template <typename T, typename... Params>
  friend kj::Own<T> kj::refcounted(Params&&... params);
  friend class GcVisitor;
  template <typename, typename...>
  friend class TypeWrapper;
  template <typename, typename>
  friend class ResourceWrapper;
  template <typename>
  friend class ObjectWrapper;
  template <typename>
  friend class SelfPropertyReader;
};

template <typename T>
class Ref {
  // Ref<T> is a reference to a resource type (a type with a JSG_RESOURCE_TYPE block) living on
  // the V8 heap.
  //
  // Use Ref<T> when you want a long-lived reference to such a type. If you only need a reference
  // that lasts until your method returns, you can specify the parameter type `T&` instead, which
  // is more efficient. Use Ref<T> when you need to keep the reference longer than that.
  //
  // WARNING: When storing Ref<T> in a C++ object that itself is referenced from the JS heap,
  // you must implement GC visitation; see GcVisitor, below.
  //
  // It is safe to destroy a jsg::Ref<T> object outside of the isolate lock. In this case,
  // the underlying V8 handles will be added to a queue, to be destroyed the next time a thread
  // locks the isolate. This means their destruction is non-deterministic, but that is true of V8
  // objects anyway, due to the GC.
  //
  // Move construction and move assignment of strong jsg::Ref<T>s is well-defined even without
  // holding the isolate lock. That is, it is safe to move Refs unless you have implemented GC
  // visitation for them. Moving jsg::Ref<T>s which are reachable via GC vistation is undefined
  // behavior outside of an isolate lock.

public:
  Ref(decltype(nullptr)): strong(false) {}
  Ref(Ref&& other): inner(kj::mv(other.inner)), strong(true) {
    if (other.strong) {
      other.strong = false;
    } else {
      inner->addStrongRef();
    }
  }
  explicit Ref(kj::Own<T> innerParam): inner(kj::mv(innerParam)), strong(true) {
    // Upgrade a KJ allocation to a Ref. This is useful if you want to allocate the object outside
    // the isolate lock and then bring it in later. The object must be allocated with
    // kj::refcounted. Once the Ref is constructed, the refcount is protected by the isolate lock
    // going forward; you can no longer add or remove refs outside the lock.
    inner->addStrongRef();
  }
  template <typename U, typename = kj::EnableIf<kj::canConvert<U&, T&>()>>
  Ref(Ref<U>&& other): inner(kj::mv(other.inner)), strong(true) {
    if (other.strong) {
      other.strong = false;
    } else {
      inner->addStrongRef();
    }
  }
  template <typename U>
  Ref& operator=(Ref<U>&& other) {
    destroy();
    inner = kj::mv(other.inner);
    strong = true;
    if (other.strong) {
      other.strong = false;
    } else {
      inner->addStrongRef();
    }
    return *this;
  }
  ~Ref() noexcept(false) {
    destroy();
  }
  KJ_DISALLOW_COPY(Ref);

  T& operator*() { return *inner; }
  T* operator->() { return inner.get(); }
  T* get() { return inner.get(); }

  Ref addRef() & {
    return Ref(kj::addRef(*inner));
  }
  Ref addRef() && = delete;  // would be redundant

  kj::Maybe<v8::Local<v8::Object>> tryGetHandle(v8::Isolate* isolate) {
    // If the object has a JS wrapper, return it. Note that the JS wrapper is initialized lazily
    // when the object is first passed to JS, so you can't be sure that it exists. To reliably
    // get a handle (creating it on-demand if necessary), use a TypeHandler<Ref<T>>.
    return inner->tryGetHandle(isolate);
  }

  void attachWrapper(v8::Isolate* isolate, v8::Local<v8::Object> object) {
    // Attach a JavaScript object which implements the JS interface for this C++ object. Normally,
    // this happens automatically the first time the Ref is passed across the FFI barrier into JS.
    // This method may be useful in order to use a different wrapper type than the one that would
    // be used automatically. This method is also useful when implementing TypeWrapperExtensions.
    //
    // It is an error to attach a wrapper when another wrapper is already attached. Hence,
    // typically this should only be called on a newly-allocated object.

    inner->Wrappable::attachWrapper(isolate, object, resourceNeedsGcTracing<T>());
  }

private:
  kj::Own<T> inner;

  kj::Maybe<Wrappable&> parent;
  // If this has ever been traced, the parent object from which the trace originated. This is kept
  // for debugging purposes only -- there should only ever be one parent for a particular ref.
  //
  // This field does NOT move when the Ref moves, because it's a property of the specific Ref
  // location.

  bool strong;
  // True if the ref is currently counted in the target's strong refcount.

  void destroy() {
    if (auto ptr = inner.get(); ptr != nullptr) {
      inner->maybeDeferDestruction(strong, kj::mv(inner), static_cast<Wrappable*>(ptr));
    }
  }

  template <typename>
  friend class Ref;
  template <typename U, typename... Params>
  friend Ref<U> alloc(Params&&... params);
  template <typename U>
  friend Ref<U> _jsgThis(U* obj);
  template <typename, typename>
  friend class ResourceWrapper;
  template <typename>
  friend class ObjectWrapper;
  friend class GcVisitor;
};

template <typename T, typename... Params>
Ref<T> alloc(Params&&... params) {
  return Ref<T>(kj::refcounted<T>(kj::fwd<Params>(params)...));
}

template <typename T>
Ref<T> _jsgThis(T* obj) {
  return Ref<T>(kj::addRef(*obj));
}

#define JSG_THIS (::workerd::jsg::_jsgThis(this))

template <typename T>
class MemoizedIdentity {
  // Holds a value of type `T` and allows it to be passed to JavaScript multiple times, resulting
  // in exactly the same JavaScript object each time (will compare equal using `===`). You may
  // pass `MemoizedIdentity<T>` by reference, e.g. you could define a method of a JSG_RESOURCE_TYPE
  // which returns `MemoizedIdentity<T>&`, returning a reference to a member of the object.
  //
  // Note that you don't need to wrap `jsg::Ref<T>` this way, as it already has the property that
  // only one wrapper will be created. `MemoizedIdentity` can wrap any type that is convertible to
  // JavaScript, including types that are otherwise pass-by-value.

public:
  inline MemoizedIdentity(T value): value(kj::mv(value)) {}

  inline MemoizedIdentity& operator=(T value) {
    this->value = kj::mv(value);
    return *this;
  }

  void visitForGc(GcVisitor& visitor);

private:
  kj::OneOf<T, Value> value;

  template <typename TypeWrapper>
  friend class MemoizedIdentityWrapper;
};

template <typename T>
struct Identified {
  // Accept this type from JavaScript when you want to receive an object's identity in addition to
  // unwrapping it. This is useful, for example, if you need to be able to recognize when the
  // application passes in the same object again later.
  //
  // `T` must be a type whose JavaScript representation is an Object (including Functions), since
  // other types do not have a notion of identity-equality.

  HashableV8Ref<v8::Object> identity;
  // Handle to the original object.

  T unwrapped;
  // The object's unwrapped value.
};

class Name final {
  // jsg::Name represents a value that is either a string or a v8::Symbol. It is most useful for
  // use in APIs that can accept both interchangeably.
  //
  // Name implements hashCode() so it is suitable for use as a key in kj::HashMap, etc.
public:
  explicit Name(kj::String string);
  explicit Name(kj::StringPtr string);
  explicit Name(Lock& js, v8::Local<v8::Symbol> symbol);
  KJ_DISALLOW_COPY(Name);
  Name(Name&&) = default;
  Name& operator=(Name&&) = default;

  inline int hashCode() const { return hash; }

private:
  kj::OneOf<kj::String, V8Ref<v8::Symbol>> inner;
  int hash;

  kj::OneOf<kj::StringPtr, v8::Local<v8::Symbol>> getUnwrapped(v8::Isolate* isolate);

  template <typename TypeWrapper>
  friend class NameWrapper;

  void visitForGc(GcVisitor& visitor);
};

template <typename Signature>
class Function;
// jsg::Function<T> behaves much like kj::Function<T>, but can be passed to/from JS. It works in
// both directions: you can receive a jsg::Function from JavaScript and call it from C++, and you
// can also initialize a jsg::Function from a C++ lambda and pass it back to JavaScript.
//
// Since the function could be backed by JavaScript, when calling it, you must always pass
// `jsg::Lock&` as the first parameter. When implementing a `jsg::Function` using a C++ lambda,
// the lambda should similarly take `jsg::Lock&` as the first parameter. Note that this first
// parameter is not declared in the function's signature. For exmaple, `jsg::Function<int(int)>`
// declares a function that accepts a parameter of type int and returns an int. However, when
// actually calling it, you must still pass `jsg::Lock&`, with the `int` as the second parameter.
// (Of course, from the JavaScript side, the lock parameter is hidden, and the `int` is in fact
// the first parameter.)
//
// jsg::Function can be visited using a GcVisitor. If a jsg::Function is initialized from a
// C++ functor object that happens to have a public method `visitForGc(jsg::GcVisitor&)`, then
// it will arrange for that method to be called during GC tracing.
//
// Note that, obviously, a normal C++ lambda cannot have a `visitForGc()` method. So when writing
// a visitable function in C++, you have to write out a struct or class with an `operator()`
// method and a `visitForGc()` method. That's a bit of a pain, so the macro JSG_VISITABLE_LAMBDA()
// is provided to assist. This lets you write something like a lambda expression where some of the
// captured variables can be GC visited. Example:
//
//     jsg::Function<void(int)> myFunc =
//         JSG_VISITABLE_LAMBDA((foo = getFoo(), bar, &baz),
//                               (foo, baz.handle),
//                               (jsg::Lock& js, int param) {
//       // ... body of function ...
//     });
//
// The first parameter to JSG_VISITABLE_LAMBDA is your capture list, in exactly the syntax that a
// regular lambda would use, except in parentheses instead of square brackets. The second
// parameter is a parenthesized list of visitation expressions. This will literally be used as a
// parameter list to `gcVisitor.visit()`, e.g. in the above example
// `gcVisitor.visit(foo, baz.handle)` will be called when visited. Finally, the third parameter
// is the rest of the lambda expression -- parameter list followed by body block.

template <typename T>
class Constructor;
// Use this to unwrap a JavaScript function that should be called as a constructor (with `new`).
// The return type in this case is the constructed type. `Constructor` is a subclass of `Function`;
// it can be used in all the same ways.

template <typename T>
class Promise;
// jsg::Promise<T> wraps a JavaScript promise. Use it when you want to pass Promises to or from
// JavaScript.
//
// jsg::Promise<T> offers a `.then()` method which looks a lot like kj::Promise<T>'s similar
// function, except that you must pass `Lock&` to it, and it passes `Lock&` back to the callback:
//
//     Promise<int> promise = ...;
//     Promise<kj::String> promise2 = promise.then(js,
//         [](Lock& js, int val) { return kj::str(val); })
//
// Unlike kj::Promise, jsg::Promises run on the V8 microtask loop, NOT on the KJ event loop. That
// implies that the isolate is already locked and active during callbacks, and control does not
// return to the KJ event loop at all if a promise continuation is immediately runnable.
//
// `.catch_()` and two-argument `.then()` are supported. Thrown exceptions are represented using
// `jsg::Value`, since technically JavaScript allows throwing any type.
//
// The type T does not have to be convertable to/from JavaScript unless a Promise<T> is actually
// passed to/from JavaScript. That is, you can have an intermediate Promise<U> where U is a type
// that has no JavaScript representation. What actually happens is, when a Promise<T> is passed
// from JS into C++, JSG adds a .then() which unwraps the value T, and when a Promise<T> is
// passed back to JS, JSG adds a .then() to wrap the value again.
//
// If the type T is GC visitable (i.e. it is a type that you could pass to GcVisitor::visit()),
// then the system will arrange to correctly visit it when the T is wrapped in a Promise.
// Additionally, if a continuation function passed to `.then()` is GC-visitable, it will similary
// be visited. JSG_VISITABLE_LAMBDA is a useful in conjunction with `.then()` (see jsg::Function,
// above).
//
// Unlike KJ promises, dropping a jsg::Promise does not cancel it. However, like a KJ promise,
// a jsg::Promise can only have `.then()` called on it once; the continuation consumes the value.
// This is so that pass-by-move C++ types can safely be passed through jsg::Promises. Of course,
// once returned to JavaScript, JS code is free to call `.then()` as many times as it wants; this
// restriction only applies to calling `.then()` in C++.
//
// When a JSG method returns a Promise, the system ensures that the object on which the method
// was called will not be GC'd until the Promise resolves (or is itself GC'd, indicating it will
// never resolve). This is a convenience so that method implementations that return promises do
// not need to carefully capture a reference to `JSG_THIS`.
//
// You can construct an immediate Promise value using jsg::resolvedPromise() and
// jsg::rejectedPromise() (see below).
//
// You can also create a promise/resolver pair:
//
//     auto [promise, resolver] = js.newPromiseAndResolver<kj::String>();
//     resolver.resolve(kj::str(foo));
//
// The Promise exposes a markAsHandled() API that will mark JavaScript Promise such that rejections
// are not reported to the isolate's unhandled rejection tracking mechanisms. Importantly, any then
// then() or catch_() continuation on either type will return an unhandled Promise. But, any
// whenResolved() continuation, and any type handler continuations added internally will be
// automatically marked handled. Use of markAsHandled() should be rare. It is largely used by Web
// Platform APIs in certain cases where consumption of a promise is optional, or where a promise
// rejection is likely to be surfaced via multiple promises (and therefore only needs to be handled
// once).

template <typename T>
struct PromiseResolverPair;

template <typename T> struct IsPromise_ { static constexpr bool value = false; };
template <typename T> struct IsPromise_<Promise<T>> { static constexpr bool value = true; };
template <typename T> constexpr bool isPromise() { return IsPromise_<T>::value; }
// Convenience template to detect a `jsg::Promise` type.

template <typename T> struct RemovePromise_ { typedef T Type; };
template <typename T> struct RemovePromise_<Promise<T>> { typedef T Type; };
template <typename T> using RemovePromise = typename RemovePromise_<T>::Type;
// Convenience template to strip off `jsg::Promise`.

template <typename Func, typename T, bool passLock>
struct ReturnType_;
template <typename Func, typename T>
struct ReturnType_<Func, T, false> {
  typedef decltype(kj::instance<Func>()(kj::instance<T>())) Type;
};
template <typename Func, typename T>
struct ReturnType_<Func, T, true> {
  typedef decltype(kj::instance<Func>()(kj::instance<Lock&>(), kj::instance<T>())) Type;
};
template <typename Func>
struct ReturnType_<Func, void, false> {
  typedef decltype(kj::instance<Func>()()) Type;
};
template <typename Func>
struct ReturnType_<Func, void, true> {
  typedef decltype(kj::instance<Func>()(kj::instance<Lock&>())) Type;
};

template <typename Func, typename T, bool passLock = false>
using ReturnType = typename ReturnType_<Func, T, passLock>::Type;
// Convenience template to calculate the return type of a function when passed parameter type T.
// `T = void` is understood to mean no parameters.

template <typename Func, typename Param, bool passLock>
using PromiseForResult = Promise<RemovePromise<ReturnType<Func, Param, passLock>>>;
// Convenience template to produce a promise for the result of calling a function with the given
// parameter type. This wraps the function's result type in `jsg::Promise` UNLESS the function
// already returns a `jsg::Promise`, in which case the type is unchanged.

template <typename T>
class JsContext {
  // Reference to a JavaScript context whose global object wraps a C++ object of type T. This is
  // similar to Ref but not the same, since JsContext provides access to the Context itself,
  // which is more than just the global object.

public:
  JsContext(v8::Local<v8::Context> handle, Ref<T> object)
      : handle(handle->GetIsolate(), handle), object(kj::mv(object)) {}
  JsContext(JsContext&&) = default;
  KJ_DISALLOW_COPY(JsContext);

  T& operator*() { return *object; }
  T* operator->() { return object.get(); }

  v8::Local<v8::Context> getHandle(v8::Isolate* isolate) {
    return handle.Get(isolate);
  }

private:
  v8::Global<v8::Context> handle;
  Ref<T> object;
};

class BufferSource;

constexpr bool hasPublicVisitForGc_(...) { return false; }
template <typename T, typename = decltype(&T::visitForGc)>
constexpr bool hasPublicVisitForGc_(T*) { return true; }

template <typename T>
constexpr bool hasPublicVisitForGc() { return hasPublicVisitForGc_((T*)nullptr); }

class GcVisitor {
  // Visitor used during garbage collection. Any resource class that holds `Ref`s should
  // implement GC visitation by declaring a private method like:
  //
  //     private:
  //       void visitForGc(GcVisitor& visitor);
  //
  // In this method, call visitor.visit() on each `Ref` owned by the object.
  //
  // A `visitForGc()` method does NOT need to handle visiting superclasses. The JSG framework will
  // automatically discover the presence of `visitForGc()` in each class in the hierarchy and will
  // arrange for them all to be called. (Thus, when adding a new `visitForGc()` method to a class
  // that has many subclasses, there is no need to update the subclasses.)
  //
  // Functors (freestanding functions/callbacks/lambdas, not declared as resources) can also
  // implement GC visitation. To do so, implement the function as a struct with `operator()`, and
  // also give the function a `visitForGc()` method. In this case, `visitForGc()` must be public.
  //
  // GC visitation is optional. If your type owns no `Ref`s, it can skip implementing
  // `visitForGc()`. You can also omit `visitForGc()` if you don't care about the possibility of
  // reference cycles. Any `Ref` which is not explicitly visited will not be eligible for
  // garbage collection at all. Hence, failure to implement proper visitation may lead to memory
  // leaks, but NOT to use-after-free.
  //
  // Note that GC visitation technically only collects JavaScript objects, including wrapper
  // objects. C++ objects will not be collected if they contain reference cycles entirely in C++
  // land. That is, if you have two C++ objects that contain `Ref`s to each other, and you
  // implement GC visitation, the JavaScript wrapper objects wrapping these C++ objects will be
  // collected, but the C++ objects will not -- a `Ref` can never becomes "dangling", and
  // therefore the C++ objects cannot be destroyed because there's no correct order in which to
  // destroy them. To avoid this situation, make sure your C++ objects have clear ownership, so
  // that the reference graph is a DAG, just like you always would in C++.

public:
  template <typename T>
  void visit(Ref<T>& ref) {
    ref.inner->visitRef(*this, ref.parent, ref.strong);
  }

  template <typename T>
  void visit(kj::Maybe<Ref<T>>& maybeRef) {
    KJ_IF_MAYBE(ref, maybeRef) {
      visit(*ref);
    }
  }

  void visit(Data& data);

  void visit(kj::Maybe<Data>& maybeData) {
    KJ_IF_MAYBE(data, maybeData) {
      visit(*data);
    }
  }

  template <typename T>
  void visit(V8Ref<T>& value) {
    visit(static_cast<Data&>(value));
  }

  template <typename T>
  void visit(kj::Maybe<V8Ref<T>>& maybeValue) {
    KJ_IF_MAYBE(value, maybeValue) {
      visit(*value);
    }
  }

  void visit(BufferSource& bufferSource);

  template <typename T, typename = kj::EnableIf<hasPublicVisitForGc<T>()>()>
  void visit(T& supportsVisit) {
    supportsVisit.visitForGc(*this);
  }

  template <typename T, typename = kj::EnableIf<hasPublicVisitForGc<T>()>()>
  void visit(kj::Maybe<T>& maybeSupportsVisit) {
    KJ_IF_MAYBE(supportsVisit, maybeSupportsVisit) {
      supportsVisit->visitForGc(*this);
    }
  }

  void visit() {}

  template <typename T, typename U, typename... Args>
  void visit(T& t, U& u, Args&... remaining) {
    visit(t);
    visit(u, kj::fwd<Args&>(remaining)...);
  }

  void visitAll(auto& collection) {
    for (auto& item : collection) {
      visit(item);
    }
  }

private:
  Wrappable& parent;
  kj::Maybe<cppgc::Visitor&> cppgcVisitor;

  explicit GcVisitor(Wrappable& parent, kj::Maybe<cppgc::Visitor&> cppgcVisitor)
      : parent(parent), cppgcVisitor(cppgcVisitor) {}
  KJ_DISALLOW_COPY_AND_MOVE(GcVisitor);

  friend class Wrappable;
  friend class Object;
  friend class HeapTracer;
};

constexpr bool isGcVisitable_(...) { return false; }
template <typename T, typename = decltype(kj::instance<GcVisitor>().visit(kj::instance<T&>()))>
constexpr bool isGcVisitable_(T*) { return true; }

template <typename T>
constexpr bool isGcVisitable() { return isGcVisitable_((T*)nullptr); }

template <typename T>
class TypeHandler {
  // TypeHandler translates between V8 values and local values for a particular type T.
  //
  // When you define a function or method that is to be wrapped by V8, you can append TypeHandler
  // references to your argument list, and they will automatically be filled in by the caller.
  // This allows you to manually manage objects of this type in your code. For example, you could
  // use this to manually test two different possible input types:
  //
  //     void myMethod(v8::Local<v8::Value> handle,
  //                   const TypeHandler<MyType1>& wrapper,
  //                   const TypeHandler<MyType2>& wrapper) {
  //       KJ_IF_MAYBE(value1, wrapper.tryUnwrap(handle)) {
  //         value1->someMyType1Method();
  //       } KJ_IF_MAYBE(value2, wrapper.tryUnwrap(handle)) {
  //         value2->someMyType2Method();
  //       }
  //     }
  //
  // To use a JSG_RESOURCE_TYPE in the TypeHandler, it must be listed in your isolate type's
  // JSG_DECLARE_ISOLATE_TYPE declaration. See JSG_DECLARE_ISOLATE_TYPE in setup.h for info.
  // For resource types, also need to wrap in Ref, i.e. `TypeHandler<jsg::Ref<T>>`.

public:
  // ---------------------------------------------------------------------------
  // Interface for value types (i.e. types not declared using JSG_RESOURCE_TYPE).
  //
  // This includes builtin types, e.g. `double` or `kj::String`.
  //
  // These methods will fail for resource types.

  virtual v8::Local<v8::Value> wrap(Lock& js, T value) const = 0;
  // Wrap by value.

  virtual kj::Maybe<T> tryUnwrap(Lock& js, v8::Local<v8::Value> handle) const = 0;
  // Unwrap by value. Returns null if not the right type.
};

template <typename T>
class PropertyReflection {
  // Utility that allows C++ code in a resource type to examine properties that have been added to
  // its JavaScript wrapper.
  //
  // To use this, add a member of type `PropertyReflection<T>` to your resource type, then after
  // your JSG_RESOURCE_TYPE block (NOT inside it; at the class scope), write
  // `JSG_REFLECTION(name)`. You will then be able to use the reflection to read properties
  // set on the JavaScript side, interpreting them as the type `T`.
  //
  //     class Foo: public jsg::Object {
  //     public:
  //       ...
  //       JSG_RESOURCE_TYPE(EventTarget) {
  //         ...
  //       }
  //       JSG_REFLECTION(intReader, stringReader);
  //     private:
  //       PropertyReflection<int> intReader;
  //       PropertyReflection<kj::String> stringReader;
  //     }
  //
  // PropertyReflection's trick is that it isn't initialized until the JavaScript wrapper is
  // created. Until that point, get() just always returns nullptr.
  //
  // PropertyReflection's main use case is reading event handler `onfoo` properties. That is,
  // traditionally, instead of using `obj.addEventListener("foo", func)` to register an event
  // handler, you can also do `obj.onfoo = func`.

public:
  kj::Maybe<T> get(Lock& js, kj::StringPtr name);
  kj::Maybe<T> get(v8::Isolate* isolate, kj::StringPtr name) {
    // Read the property of this object called `name`, unwraping it as type `T`.
    v8::HandleScope scope(isolate);
    KJ_IF_MAYBE(s, self) {
      KJ_IF_MAYBE(h, s->tryGetHandle(isolate)) {
        return unwrapper(isolate, *h, name);
      }
    }
    return nullptr;
  }

  // TODO(someday): Support for reading Symbols and Privates?

private:
  kj::Maybe<Wrappable&> self;

  typedef kj::Maybe<T> Unwrapper(v8::Isolate*, v8::Local<v8::Object> object, kj::StringPtr name);
  Unwrapper* unwrapper = nullptr;

  template <typename, typename...>
  friend class TypeWrapper;
};

struct Unimplemented {};
// Use this type to mark APIs that are not implemented. Attempts to use the API will throw an
// exception.
// - Use Unimplemented as a method parameter type or struct field type to mark that
//   parameter/field unimplemented; only the value `undefined` will be allowed.
// - Use Unimplemented as the return type of a method to mark the whole method unimplemented.
//   Have the method body simply return `Unimplemented()`.
//
// TODO(someday): We should consider making it easier for people to probe features by doing
//   `if (obj.someMember)`. Currently this check would pass for methods and would throw an
//   exception for properties. Is it possible for us to hook into the V8 feature where there are
//   special values of `undefined` that augment the error message thrown if they are used?

using WontImplement = Unimplemented;
// Use to mark APIs that are not just unimplemented, but that we don't plan to implement, e.g.
// standard ServiceWorker APIs that don't make sense for Workers.

template <typename T>
concept CoercibleType =
    kj::isSameType<kj::String, T>() ||
    kj::isSameType<bool, T>() ||
    kj::isSameType<double, T>();
// When updating this list, be sure to keep the corresponding checks in the NonCoercibleWrapper
// class in value.h updated as well.

template <CoercibleType T>
struct NonCoercible {
  T value;
};
// By default types in JavaScript can be implicitly converted to other types as needed. This
// can lead to surprising results. For instance, passing null into an API method that accepts
// string will have the null coerced into the string value "null". The NonCoercible type can
// be used to disable automatic type coercion in APIs. For instance, NonCoercible<kj::String>
// will ensure that any value other than a string will be rejected with a TypeError.
//
// Here, T can be only one of several types that support coercion:
//
// * kj::String (value must be a string)
// * bool (value must be a boolean)
// * double (value must be a number)
//
// It should be pointed out that using NonCoercible<T> runs counter to Web IDL and general
// Web Platform API best practices, which use type coercion fairly often. However, in certain
// Cloudflare-specific APIs, automatic coercion can cause surprising developer experience
// issues. Only use NonCoercible if you have a good reason to disable coercion. When in
// doubt, don't use it.

// -----------------------------------------------------------------------------

template <typename T>
struct Sequence;
// A Sequence<T> in C++ corresponds to a Sequence IDL type. A sequence is a list of values
// that may or may not be an array. The key difference between the kj::Array mapping in
// JSG and a jsg::Sequence, is that the jsg::Sequence can be initialized from any object
// that exposes an @@iterable symbol. However, when a Sequence is surfaced back up to
// JavaScript, it will always be an array.
//
// At the C++ level, the Sequence itself is just a kj::Array<Value>.
//
// Both jsg::Sequence and jsg::Generator provide the ability to work with synchronous
// iterable/generator objects. The key difference is that jsg::Sequence will always
// produce a kj::Array of the elements, does not allow for early termination of the
// iteration, and does not provide access to the return value. jsg::Generator, on the
// other hand, allows performing an action on each individual item, terminating the
// iteration early, and retrieving the generators final return value, if any.

template <typename T>
class Generator;
// jsg::Generator wraps a JavaScript synchronous generator.
//
// jsg::Generator offers a `.forEach()` method that will invoke a callback function for
// each individual item produced by the generator:
//
//   Generator<int> generator = ...;
//   generator.forEach(js, [](Lock& js, int val, GeneratorContext<T> context) {
//     // Do something with val.
//     // To exit early from the iteration, either call `context.return_()`,
//     // which will call the `.return()` method on the underlying generator,
//     // or throw a JavaScript exception, which will call the `.throw()`
//     // method on the underlying generator.
//   });
//
// The Generator<T> is intended only to be used when receiving a Generator object as
// a parameter. Instances of Generator<T> cannot be passed back out to JavaScript. Refer
// to the documentation for JSG_ITERATOR to see how to create and pass Generator/Iterable
// objects back out to JavaScript.
//
// The `.forEach()` method is fully synchronous and will fully consume the generator
// before it returns. Calling `.forEach()` a second time on the generator will return
// immediately as a non-op.

template <typename T>
class AsyncGenerator;
// The jsg::AsyncGenerator wraps a JavaScript asynchronous generator.
//
// The jsg::AsyncGenerator is similar to jsg::Generator except that it supports
// async iteration over the individual elements produced by the generator. The
// `.forEach()` method returns a `Promise<kj::Maybe<T>>>` that is resolved once the
// generator as been fully consumed. The callback passed in to `.forEach()` must
// also return a `Promise<void>` that is resolved whenever the item has been consumed
// and the iterator should advance to the next item.
//
//   AsyncGenerator<int> generator = ...;
//   generator.forEach(js, [](Lock& js, int val, GeneratorContext<T> context) {
//     // Do something with val.
//     // To exit early from the iteration, either call `context.return_()`,
//     // which will call the `.return()` method on the underlying generator,
//     // or throw a JavaScript exception, which will call the `.throw()`
//     // method on the underlying generator.
//     return js.resolvedPromise();
//   }).then(js, [](Lock&, kj::Maybe<T>) { KJ_DBG("DONE!"); });
//
// The `.forEach()` method will fully consume the generator, returning a Promise
// that is resolved once the generator completes. Calling `.forEach()` a second
// time on the generator will return an immediately resolved promise.

template <typename T>
class GeneratorContext;
// The jsg::GeneratorContext is used with both jsg::Generator and jsg::AsyncGenerator
// to allow for early termination of the generator iteration.

// -----------------------------------------------------------------------------

struct JsgConfig {
  bool noSubstituteNull = false;
};

// -----------------------------------------------------------------------------

class IsolateBase;
template <typename TypeWrapper>
class Isolate;
// Defined in setup.h -- most code doesn't need to use these directly.

class AsyncContextFrame;

#define JSG_PRIVATE_SYMBOLS(V)       \
  V(ASYNC_CONTEXT, "asyncContext")
// Defines the enum values for Lock::PrivateSymbols.

class Lock {
  // Represents an isolate lock, which allows the current thread to execute JavaScript code within
  // an isolate. A thread must lock an isolate -- obtaining an instance of `Lock` -- before it can
  // manipulate JavaScript objects or execute JavaScript code inside the isolate.
  //
  // The `Lock` interface also provides access to basic JavaScript functionality, such as the
  // ability to construct basic JS values, throw and catch errors, etc.
  //
  // By convention, all functions which manipulate JavaScript take `Lock& js` as their first
  // parameter. A `Lock&` reference must never be stored as an object member nor captured in a
  // lambda, as `Lock`s are always constructed on the stack and so their lifetime is never
  // guaranteed beyond the end of the function call.
  //
  // Methods declared with JSG_METHOD and similar macros may optionally take a `Lock&` as the
  // first parameter. Template magic will automatically discover if the parameter is present and
  // will populate it. Such methods are always invoked under lock whether or not they have a
  // `Lock&` parameter, but it is recommended that you declare the parameter if the function
  // touches the JS heap in any way. This way, if someone wants to call the method directly from
  // C++, they know whether a lock is required.
  //
  // To create a lock in the first place, you have to create a specific instance of
  // Isolate<TypeWrapper>::Lock. Usually this is only done in top-level code, and the Lock is
  // passed down to everyone else from there. See setup.h for details.

public:
  v8::Isolate* const v8Isolate;
  // The underlying V8 isolate, useful for directly calling V8 APIs. Hopefully, this is rarely
  // needed outside JSG itself.

  static Lock& from(v8::Isolate* v8Isolate) {
    // Get the current Lock for the given V8 isolate. Segfaults if the isolate is not locked.
    //
    // This method is intended to be used in callbacks from V8 that pass an isolate pointer but
    // don't provide any further context. Most code should rely on the caller passing in a `Lock&`.

    return *reinterpret_cast<Lock*>(v8Isolate->GetData(2));
  }

  Value parseJson(kj::StringPtr text);
  template <typename T>
  kj::String serializeJson(V8Ref<T>& value) { return serializeJson(value.getHandle(*this)); }
  template <typename T>
  kj::String serializeJson(V8Ref<T>&& value) { return serializeJson(value.getHandle(*this)); }

  // ---------------------------------------------------------------------------
  // Exception-related stuff

  Value exceptionToJs(kj::Exception&& exception);
  // Converts the KJ exception to a JS exception. If the KJ exception is a tunneled JavaScript
  // error, this reproduces the original error. If it is not a tunneled error, then it is treated
  // as an internal error: the KJ exception message is logged to stderr, and a JavaScript error
  // is returned with a generic description.

  kj::Exception exceptionToKj(Value&& exception);
  // Encodes the given JavaScript exception into a KJ exception, formatting the description in
  // such a way that hopefully exceptionToJs() can reproduce something equivalent to the original
  // JavaScript error.

  [[noreturn]] void throwException(Value&& exception);
  // Throws a JavaScript exception. The exception is scheduled on the isolate, and then an
  // instance of `JsExceptionThrown` is thrown in C++. All places where JavaScript calls into C++
  // via JSG understand how to handle this and propagate the exception back to JavaScript.

  [[noreturn]] void throwException(kj::Exception&& exception) {
    throwException(exceptionToJs(kj::mv(exception)));
  }

  template <typename Func, typename ErrorHandler>
  auto tryCatch(Func&& func, ErrorHandler&& errorHandler) -> decltype(func()) {
    // Invokes `func()` synchronously, catching exceptions. In the event of an exception,
    // `errorHandler()` will be called, passing the exception as type `jsg::Value`.
    //
    // KJ exceptions are also caught and will be converted to JS exceptions using exceptionToJs().
    //
    // Some kinds of exceptions explicitly will not be caught:
    // - Exceptions where JavaScript execution cannot continue, such as the "uncatchable exception"
    //   produced by IsolateBase::terminateExecution().
    // - C++ exceptions other than `kj::Exception`, e.g. `std::bad_alloc`. These exceptions are
    //   assumed to be serious enough that they cannot be caught as if they were JavaScript errors,
    //   and instead unwind must continue until C++ catches them.
    //
    // func() and errorHandler() must return the same type; the value they return will be retuned
    // from `tryCatch()` itself.

    Value error = nullptr;

    {
      v8::TryCatch tryCatch(v8Isolate);
      try {
        return func();
      } catch (JsExceptionThrown&) {
        if (!tryCatch.CanContinue()) {
          throw;
        }
        error = Value(v8Isolate, tryCatch.Exception());
      } catch (kj::Exception& e) {
        error = exceptionToJs(kj::mv(e));
      }
    }

    // We have to make sure the `v8::TryCatch` is off the stack before invoking `errorHandler`,
    // otherwise the same `TryCatch` will catch any exceptions the error handler throws, ugh.
    return errorHandler(kj::mv(error));
  }

  // ---------------------------------------------------------------------------
  // Promise-related stuff

  template <typename T>
  PromiseResolverPair<T> newPromiseAndResolver();
  // Get a pair of a Promise<T> and a Promise<T>::Resolver that resolves the promise. You should
  // call this like:
  //
  //     auto [promise, resolver] = js.newPromiseAndResolver();

  template <typename T>
  Promise<T> resolvedPromise(T&& value);
  Promise<void> resolvedPromise();
  // Construct an immediately-resolved promise resolving to the given value.

  template <typename T>
  Promise<T> rejectedPromise(v8::Local<v8::Value> exception);
  template <typename T>
  Promise<T> rejectedPromise(jsg::Value exception);
  template <typename T>
  Promise<T> rejectedPromise(kj::Exception&& exception);
  // Construct an immediately-rejected promise throwing the given exception.

  template <class Func>
  PromiseForResult<Func, void, false> evalNow(Func&& func);
  // Like `kj::evalNow()`, but returns a jsg::Promise for the result. Synchronous exceptions are
  // caught and returned as a rejected promise.
  //
  // If an exception is caught as a result of TerminateExecution() being called, it is rethrown
  // to the caller, not encapsulated in a promise.
  //
  // Note `func` is NOT expected to take `Lock&` as a parameter, as normally func should be a lambda
  // that captures `[&]`, so will capture the caller's lock reference. Capturing the lock here is
  // allowed since `func` is invoked synchronously.

  // ---------------------------------------------------------------------------
  // Name/Symbol stuff

  Name newSymbol(kj::StringPtr symbol);
  // Creates a Name encapsulating a new unique v8::Symbol.

  Name newSharedSymbol(kj::StringPtr symbol);
  // Creates a Name encapsulating a name from the global symbol registry.
  // Equivalent to Symbol.for(symbol) in JavaScript.

  Name newApiSymbol(kj::StringPtr symbol);
  // Similar to newSharedSymbol except that it uses a separate isolate registry
  // that is not accessible by JavaScript.

  // ---------------------------------------------------------------------------
  // Logging stuff

  inline bool areWarningsLogged() const { return warningsLogged; }

  void logWarning(kj::StringPtr message);
  // Emits the warning only if there is anywhere for the log to go (for instance,
  // if debug logging is enabled or the inspector is being used).

  // TODO(later): Add the other log variants from IoContext? eg. logWarningOnce,
  // logErrorOnce, logUncaughtException, etc.

  // ---------------------------------------------------------------------------
  // v8 Local handle related stuff
  // TODO(cleanup): Direct use of v8::Local handles is discouraged and is something we are trying
  // to move away from. However, there are still plenty of cases where we need to do so. The
  // methods here help avoid directly using v8::Isolate and serve as an interim until we can
  // eliminate direct use as much as possible.

  v8::Local<v8::Value> v8Undefined();

  v8::Local<v8::Value> v8Error(kj::StringPtr message);
  v8::Local<v8::Value> v8TypeError(kj::StringPtr message);

  template <typename T>
  V8Ref<T> v8Ref(v8::Local<T> local);
  Data v8Data(v8::Local<v8::Data> data);

  kj::String serializeJson(v8::Local<v8::Value> value);

  v8::Local<v8::String> wrapString(kj::StringPtr text);
  virtual v8::Local<v8::ArrayBuffer> wrapBytes(kj::Array<byte> data) = 0;
  virtual v8::Local<v8::Function> wrapSimpleFunction(v8::Local<v8::Context> context,
      jsg::Function<void(const v8::FunctionCallbackInfo<v8::Value>& info)> simpleFunction) = 0;
  virtual v8::Local<v8::Function> wrapReturningFunction(v8::Local<v8::Context> context,
      jsg::Function<v8::Local<v8::Value>(const v8::FunctionCallbackInfo<v8::Value>& info)> returningFunction) = 0;
  // A variation on wrapSimpleFunction that allows for a return value. While the wrapSimpleFunction
  // implementation passes the FunctionCallbackInfo into the called function, any call to
  // GetReturnValue().Set(...) to specify a return value will be ignored by the FunctorCallback
  // wrapper. The wrapReturningFunction variation forces the wrapper to use the version that
  // pays attention to the return value.
  // TODO(later): See if we can easily combine wrapSimpleFunction and wrapReturningFunction
  // into one.

  bool toBool(v8::Local<v8::Value> value);
  virtual kj::String toString(v8::Local<v8::Value> value) = 0;
  virtual jsg::Dict<v8::Local<v8::Value>> toDict(v8::Local<v8::Value> value) = 0;
  // Convenience methods to unwrap various types of V8 values. All of these could be done manually
  // via the V8 API, but these methods are much easier.

  // ---------------------------------------------------------------------------
  // Setup stuff

  void setAllowEval(bool allow);
  // Use to enable/disable dynamic code evaluation (via eval(), new Function(), or WebAssembly).

  void setCaptureThrowsAsRejections(bool capture);
  void setCommonJsExportDefault(bool exportDefault);

  using Logger = void(Lock&, kj::StringPtr);
  void setLoggerCallback(kj::Function<Logger>&& logger);

  // ---------------------------------------------------------------------------
  // Misc. Stuff

  void requestGcForTesting() const;
  // Sends an immediate request for full GC, this function is to ONLY be used in testing, otherwise
  // it will throw. If a need for a minor GC is needed look at the call in jsg.c++ and the
  // implementation in setup.c++. Use responsibly.

  kj::StringPtr getUuid() const;
  // Returns a random UUID for this isolate instance. This is largely intended for logging and
  // diagnostic purposes.

#define V(name, _) name,
  enum PrivateSymbols {
    JSG_PRIVATE_SYMBOLS(V)
    SYMBOL_COUNT,
    // The SYMBOL_COUNT is a special token used to size the array for storing the
    // symbol instances. It must always be the last item in the enum. To add private
    // symbols, add values to the JSG_PRIVATE_SYMBOLS define.
  };
#undef V

  v8::Local<v8::Private> getPrivateSymbolFor(PrivateSymbols symbol);

private:
  friend class IsolateBase;
  template <typename TypeWrapper>
  friend class Isolate;

  Lock(v8::Isolate* v8Isolate);
  ~Lock() noexcept(false);

  v8::Locker locker;
  v8::Isolate::Scope scope;

  void* previousData;

  bool warningsLogged;
};

// =======================================================================================
// inline implementation details

template <typename T>
inline kj::Maybe<T> PropertyReflection<T>::get(Lock& js, kj::StringPtr name) {
  return get(js.v8Isolate, name);
}

template <typename T>
inline V8Ref<T> Lock::v8Ref(v8::Local<T> local) {
  return V8Ref(v8Isolate, local);
}

inline Data Lock::v8Data(v8::Local<v8::Data> local) {
  return Data(v8Isolate, local);
}

inline v8::Local<v8::Value> Lock::v8Undefined() {
  return v8::Undefined(v8Isolate);
}

inline Data Data::addRef(jsg::Lock& js) {
  return Data(js.v8Isolate, getHandle(js.v8Isolate));
}

template <typename T>
inline V8Ref<T> V8Ref<T>::addRef(jsg::Lock& js) {
  return js.v8Ref(getHandle(js.v8Isolate));
}

v8::Local<v8::Value> deepClone(v8::Local<v8::Context> context, v8::Local<v8::Value> value);
// Defined in util.c++.

template <typename T>
V8Ref<T> V8Ref<T>::deepClone(jsg::Lock& js) {
  return js.v8Ref(jsg::deepClone(js.v8Isolate->GetCurrentContext(), getHandle(js))
      .template As<T>());
}

template <typename T>
inline HashableV8Ref<T> HashableV8Ref<T>::addRef(jsg::Lock& js) {
  return HashableV8Ref(js.v8Isolate, this->getHandle(js.v8Isolate), identityHash);
}

template <typename  T>
inline v8::Local<T> V8Ref<T>::getHandle(jsg::Lock& js) {
  return getHandle(js.v8Isolate);
}

inline v8::Local<v8::Data> Data::getHandle(jsg::Lock& js) {
  return getHandle(js.v8Isolate);
}

}  // namespace workerd::jsg

// These two includes are needed for the JSG type glue macros to work.
#include "resource.h"
#include "dom-exception.h"
#include "struct.h"
#include "promise.h"
#include "function.h"
#include "iterator.h"
