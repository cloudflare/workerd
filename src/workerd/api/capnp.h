#pragma once

#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>

#include <capnp/dynamic.h>
#include <kj/map.h>

namespace workerd::api {

template <typename TypeWrapper>
class CapnpTypeWrapper;
class CapnpCapability;
class CapnpTypeWrapperBase;

void fillCapnpFieldFromJs(capnp::DynamicStruct::Builder builder,
    capnp::StructSchema::Field field,
    v8::Local<v8::Context> context,
    v8::Local<v8::Value> jsValue);

capnp::Orphan<capnp::DynamicValue> capnpValueFromJs(
    jsg::Lock& js, capnp::Orphanage orphanage, capnp::Type type, v8::Local<v8::Value> jsValue);

class CapnpServer final: public capnp::DynamicCapability::Server {
 public:
  CapnpServer(jsg::Lock& js,
      capnp::InterfaceSchema schema,
      jsg::V8Ref<v8::Object> object,
      CapnpTypeWrapperBase& wrapper);
  ~CapnpServer() noexcept(false);

  kj::Promise<void> call(capnp::InterfaceSchema::Method method,
      capnp::CallContext<capnp::DynamicStruct, capnp::DynamicStruct> context) override;

 private:
  kj::Own<IoContext::WeakRef> ioContext;
  jsg::V8Ref<v8::Object> object;
  kj::Maybe<jsg::V8Ref<v8::Function>> closeMethod;
  CapnpTypeWrapperBase& wrapper;  // only valid if isolate is locked!

  kj::Maybe<jsg::V8Ref<v8::Function>> getCloseMethod(jsg::Lock& js);

  friend class CapnpCapability;
};

class CapnpCapability: public jsg::Object {
 public:
  CapnpCapability(capnp::DynamicCapability::Client client);
  ~CapnpCapability() noexcept(false);

  v8::Local<v8::Value> call(jsg::Lock& js,
      capnp::InterfaceSchema::Method method,
      v8::Local<v8::Value> params,
      CapnpTypeWrapperBase& wrapper);

  void close();
  jsg::Promise<kj::Maybe<jsg::V8Ref<v8::Object>>> unwrap(jsg::Lock& js);

  JSG_RESOURCE_TYPE(CapnpCapability) {
    JSG_METHOD(close);
    JSG_METHOD(unwrap);
  }

  capnp::DynamicCapability::Client getClient(jsg::Lock& js, CapnpTypeWrapperBase& wrapper);

 private:
  // Used for error messages.
  capnp::InterfaceSchema schema;

  // null if closed
  kj::Maybe<IoOwn<capnp::DynamicCapability::Client>> client;

  template <typename TypeWrapper>
  friend class CapnpTypeWrapper;
};

class CapnpTypeWrapperBase {
 public:
  virtual v8::Local<v8::Object> wrapCap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      capnp::DynamicCapability::Client value,
      kj::Maybe<jsg::Ref<CapnpCapability>&> refToInitialize = kj::none) = 0;
  virtual kj::Maybe<capnp::DynamicCapability::Client> tryUnwrapCap(
      jsg::Lock& js, v8::Local<v8::Context> context, v8::Local<v8::Value> value) = 0;

  virtual v8::Local<v8::Promise> wrapPromise(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      jsg::Promise<jsg::Value> value) = 0;
  virtual kj::Maybe<jsg::Promise<jsg::Value>> tryUnwrapPromise(
      jsg::Lock& js, v8::Local<v8::Context> context, v8::Local<v8::Value> value) = 0;
};

template <typename TypeWrapper>
class CapnpTypeWrapper: private CapnpTypeWrapperBase {
 public:
  static constexpr const char* getName(capnp::DynamicCapability::Client*) {
    return "Capability";
  }

  v8::Local<v8::Function> wrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      capnp::Schema schema) {
    auto tmpl = getCapnpTemplate(js, schema);
    return jsg::check(tmpl->GetFunction(context));
  }

  v8::Local<v8::Object> wrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      capnp::DynamicCapability::Client client,
      kj::Maybe<jsg::Ref<CapnpCapability>&> refToInitialize = kj::none) {
    auto tmpl = getCapnpTemplate(js, client.getSchema());
    auto obj = jsg::check(tmpl->InstanceTemplate()->NewInstance(context));
    auto ref = js.alloc<CapnpCapability>(kj::mv(client));
    ref.attachWrapper(js.v8Isolate, obj);
    KJ_IF_SOME(r, refToInitialize) {
      r = kj::mv(ref);
    }
    return obj;
  }

  // Wrap a specific compiled-in interface. This lets you use MyType::Client as a return type
  // in a JSG method.
  template <typename Client, typename = capnp::FromClient<Client>>
  v8::Local<v8::Object> wrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      Client client) {
    return wrap(js, context, creator, capnp::toDynamic(kj::mv(client)));
  }

  kj::Maybe<capnp::DynamicCapability::Client> tryUnwrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      capnp::DynamicCapability::Client*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto& wrapper = static_cast<TypeWrapper&>(*this);
    KJ_IF_SOME(obj,
        wrapper.tryUnwrap(js, context, handle, (CapnpCapability*)nullptr, parentObject)) {
      return obj.getClient(js, *this);
    } else {
      // Since we don't know the schema, we cannot accept an arbitrary object.
      return kj::none;
    }
  }

  // Unwrap a specific compiled-in interface. This lets you use MyType::Client as a parameter
  // in a JSG method.
  template <typename Client, typename Interface = capnp::FromClient<Client>>
  kj::Maybe<Client> tryUnwrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Client*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto expectedSchema = capnp::Schema::from<Interface>();

    auto& wrapper = static_cast<TypeWrapper&>(*this);
    KJ_IF_SOME(obj,
        wrapper.tryUnwrap(js, context, handle, (CapnpCapability*)nullptr, parentObject)) {
      capnp::DynamicCapability::Client dynamic = obj.getClient(js.v8Isolate, *this);
      if (dynamic.getSchema().extends(expectedSchema)) {
        return dynamic.as<Interface>();
      } else {
        // Incompatible interfaces.
        return kj::none;
      }
    } else if (handle->IsObject()) {
      // Treat object as a server implementation.
      auto isolate = js.v8Isolate;
      CapnpTypeWrapperBase& wrapper = TypeWrapper::from(isolate);
      capnp::DynamicCapability::Client dynamic = IoContext::current().getLocalCapSet().add(
          kj::heap<CapnpServer>(expectedSchema, handle.As<v8::Object>(), wrapper, isolate));
      return dynamic.as<Interface>();
    } else {
      return kj::none;
    }
  }

  // Not relevant for us but we must define a method with this name to satisfy TypeWrapperExtension.
  void newContext() = delete;

  template <bool isContext = false, typename Client, typename Interface = capnp::FromClient<Client>>
  v8::Local<v8::FunctionTemplate> getTemplate(jsg::Lock& js, Client*) {
    static_assert(!isContext);
    return getCapnpTemplate(js, capnp::Schema::from<Interface>());
  }

  v8::Local<v8::FunctionTemplate> getCapnpTemplate(jsg::Lock& js, capnp::Schema schema) {
    using Ret = decltype(typeConstructors)::Entry;
    return typeConstructors
        .findOrCreate(schema, [&]() -> Ret {
      return js.withinHandleScope([&]() -> Ret {
        auto handle = makeConstructor(js, schema);
        return {schema, {js.v8Isolate, handle}};
      });
    }).Get(js.v8Isolate);
  }

 private:
  kj::HashMap<capnp::Schema, v8::Global<v8::FunctionTemplate>> typeConstructors;

  // Each method callback we create needs to pack the method schema into a v8::External. But
  // v8::External can only store a pointer, and InterfaceSchema::Method is larger than a pointer.
  // So we need to allocate copies of all the `Method` objects somewhere where they'll live until
  // the isolate shuts down.
  kj::HashMap<capnp::InterfaceSchema, kj::Array<capnp::InterfaceSchema::Method>> methodSchemas;

  v8::Local<v8::FunctionTemplate> makeConstructor(jsg::Lock& js, capnp::Schema schema) {
    return js.withinHandleScope([&]() -> v8::Local<v8::FunctionTemplate> {
      // HACK: We happen to know that `Schema` is just a pointer internally, and is
      //   trivially copyable and destructible. So, we can safely stuff it directly into a
      //   v8::External by value, avoiding extra allocations.
      static_assert(sizeof(schema) == sizeof(void*));
      static_assert(__is_trivially_copyable(capnp::Schema));
      static_assert(__is_trivially_destructible(capnp::Schema));
      void* schemaAsPtr;
      memcpy(&schemaAsPtr, &schema, sizeof(schema));

      auto constructor = v8::FunctionTemplate::New(js.v8Isolate, &constructorCallback,
          v8::External::New(js.v8Isolate, schemaAsPtr, v8::kExternalPointerTypeTagDefault));

      auto prototype = constructor->PrototypeTemplate();
      auto signature = v8::Signature::New(js.v8Isolate, constructor);

      auto instance = constructor->InstanceTemplate();

      constructor->SetClassName(jsg::v8StrIntern(js.v8Isolate, schema.getShortDisplayName()));

      auto& wrapper = static_cast<TypeWrapper&>(*this);

      auto proto = schema.getProto();
      switch (proto.which()) {
        case capnp::schema::Node::FILE:
        case capnp::schema::Node::STRUCT:
        case capnp::schema::Node::ENUM:
        case capnp::schema::Node::CONST:
        case capnp::schema::Node::ANNOTATION:
          // TODO(someday): Support non-interface types.
          break;

        case capnp::schema::Node::INTERFACE: {
          // As explained in ResourceWrapper, we must have 2 internal fields, where the first one is
          // the GC visitation callback.
          instance->SetInternalFieldCount(jsg::Wrappable::INTERNAL_FIELD_COUNT);

          constructor->Inherit(
              wrapper.getTemplate(js.v8Isolate, static_cast<CapnpCapability*>(nullptr)));
          kj::HashSet<uint64_t> seen;
          addAllMethods(js, prototype, signature, schema.asInterface(), seen);
          break;
        }
      }

      for (auto nested: proto.getNestedNodes()) {
        KJ_IF_SOME(child,
            js.getCapnpSchemaLoader<api::ServiceWorkerGlobalScope>().tryGet(nested.getId())) {
          switch (child.getProto().which()) {
            case capnp::schema::Node::FILE:
            case capnp::schema::Node::STRUCT:
            case capnp::schema::Node::INTERFACE:
              constructor->Set(
                  jsg::v8StrIntern(js.v8Isolate, nested.getName()), makeConstructor(js, child));
              break;

            case capnp::schema::Node::ENUM:
            case capnp::schema::Node::CONST:
            case capnp::schema::Node::ANNOTATION:
              // These kinds are not implemented and cannot contain further nested scopes, so don't
              // generate anything at all for now.
              break;
          }
        }
      }

      return constructor;
    });
  }

  // Add all methods to the capability prototype. Since JavaScript doesn't support multiple
  // inheritance, we need to flatten all inherited methods into each interface.
  //
  // `seen` is a set of type IDs that we've visited already, so that diamond inheritance doesn't
  // lead to us double-registering methods.
  void addAllMethods(jsg::Lock& js,
      v8::Local<v8::ObjectTemplate> prototype,
      v8::Local<v8::Signature> signature,
      capnp::InterfaceSchema schema,
      kj::HashSet<uint64_t>& seen) {

    JSG_REQUIRE(seen.size() < 64, TypeError,
        "Interface inherits too many types: ", schema.getProto().getDisplayName());

    // Reverse-iterate so that in case of duplicate method names, the method from the first class
    // in the list takes precedence.
    auto supers = schema.getSuperclasses();
    for (auto i = supers.size(); i > 0; i--) {
      auto super = supers[i - 1];

      // Check if this superclass is in the `seen` set. As a slight optimization we only check this
      // before visiting a superclass, so that for a regular interface that doesn't inherit
      // anything, we never allocate the `seen` set. This assumes that inheritance is not cyclic.
      // Technically it's possible to declare cyclic inheritance (maliciously, perhaps), but in
      // that case we'll just redundantly create the methods for one type, which is not a big deal.
      uint64_t id = super.getProto().getId();
      bool isNew = false;
      seen.findOrCreate(id, [&]() {
        isNew = true;
        return id;
      });
      if (isNew) {
        addAllMethods(js, prototype, signature, super, seen);
      }
    }

    kj::ArrayPtr<capnp::InterfaceSchema::Method> methods =
        methodSchemas.findOrCreate(schema, [&]() -> decltype(methodSchemas)::Entry {
      return {schema, KJ_MAP(m, schema.getMethods()) { return m; }};
    });

    for (auto& method: methods) {
      auto name = jsg::v8StrIntern(js.v8Isolate, method.getProto().getName());
      prototype->Set(name,
          v8::FunctionTemplate::New(js.v8Isolate, &methodCallback,
              v8::External::New(js.v8Isolate, &method, v8::kExternalPointerTypeTagDefault),
              signature, 0, v8::ConstructorBehavior::kThrow));
    }
  }

  static void constructorCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    jsg::liftKj(args, [&]() {
      auto data = args.Data();
      KJ_ASSERT(data->IsExternal());
      void* schemaAsPtr = data.As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault);
      capnp::Schema schema;
      memcpy(&schema, &schemaAsPtr, sizeof(schema));

      JSG_REQUIRE(args.IsConstructCall(), TypeError, "Failed to construct '",
          schema.getShortDisplayName(),
          "': Please use the "
          "'new' operator, this object constructor cannot be called as a function.");

      auto& js = jsg::Lock::from(args.GetIsolate());
      auto obj = args.This();
      KJ_ASSERT(obj->InternalFieldCount() == jsg::Wrappable::INTERNAL_FIELD_COUNT);

      auto arg = args[0];
      JSG_REQUIRE(arg->IsObject(), TypeError, "Constructor argument for '",
          schema.getShortDisplayName(),
          "' must be an object "
          "implementing the interface.");

      CapnpTypeWrapperBase& wrapper = TypeWrapper::from(js.v8Isolate);
      capnp::DynamicCapability::Client client =
          IoContext::current().getLocalCapSet().add(kj::heap<CapnpServer>(js, schema.asInterface(),
              jsg::V8Ref<v8::Object>(js.v8Isolate, arg.As<v8::Object>()), wrapper));
      auto ptr = js.alloc<CapnpCapability>(kj::mv(client));

      ptr.attachWrapper(js.v8Isolate, obj);
    });
  }

  static void methodCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    jsg::liftKj(args, [&]() {
      auto data = args.Data();
      KJ_ASSERT(data->IsExternal());
      auto& method = *reinterpret_cast<capnp::InterfaceSchema::Method*>(
          data.As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));

      auto& js = jsg::Lock::from(args.GetIsolate());
      auto obj = args.This();
      auto& wrapper = TypeWrapper::from(js.v8Isolate);
      auto& self = jsg::extractInternalPointer<CapnpCapability, false>(js.v8Context(), obj);

      return wrapper.wrap(js, js.v8Context(), obj, self.call(js, method, args[0], wrapper));
    });
  }

  v8::Local<v8::Object> wrapCap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      capnp::DynamicCapability::Client value,
      kj::Maybe<jsg::Ref<CapnpCapability>&> refToInitialize) override {
    return wrap(js, context, kj::none, kj::mv(value), refToInitialize);
  }
  kj::Maybe<capnp::DynamicCapability::Client> tryUnwrapCap(
      jsg::Lock& js, v8::Local<v8::Context> context, v8::Local<v8::Value> value) override {
    return tryUnwrap(
        js, context, value, static_cast<capnp::DynamicCapability::Client*>(nullptr), kj::none);
  }

  v8::Local<v8::Promise> wrapPromise(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      jsg::Promise<jsg::Value> value) override {
    return static_cast<TypeWrapper&>(*this).wrap(js, context, creator, kj::mv(value));
  }
  kj::Maybe<jsg::Promise<jsg::Value>> tryUnwrapPromise(
      jsg::Lock& js, v8::Local<v8::Context> context, v8::Local<v8::Value> value) override {
    return static_cast<TypeWrapper&>(*this).tryUnwrap(
        js, context, value, static_cast<jsg::Promise<jsg::Value>*>(nullptr), kj::none);
  }
};

#define EW_CAPNP_TYPES                                                                             \
  ::workerd::api::CapnpCapability,                                                                 \
      ::workerd::jsg::TypeWrapperExtension<::workerd::api::CapnpTypeWrapper>
// The list of capnp.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
