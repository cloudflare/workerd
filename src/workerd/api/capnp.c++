#include "capnp.h"

namespace workerd::api {

// =======================================================================================
// Some code here is derived from node-capnp.
// Copyright (c) 2014-2021 Kenton Varda, Sandstorm Development Group, Inc., and contributors
// Licensed under the MIT License

#define STACK_STR(js, name, handle, sizeHint)                                                      \
  /* Read a JavaScript string, allocating it on the stack if it's small enough. */                 \
  char name##_buf[sizeHint]{};                                                                     \
  kj::Array<char> name##_heap;                                                                     \
  kj::StringPtr name;                                                                              \
  {                                                                                                \
    v8::Local<v8::String> v8str = jsg::check(handle->ToString(js.v8Context()));                    \
    char* ptr;                                                                                     \
    size_t len = v8str->Utf8LengthV2(js.v8Isolate);                                                \
    if (len < sizeHint) {                                                                          \
      ptr = name##_buf;                                                                            \
    } else {                                                                                       \
      name##_heap = kj::heapArray<char>(len + 1);                                                  \
      ptr = name##_heap.begin();                                                                   \
    }                                                                                              \
    v8str->WriteUtf8V2(js.v8Isolate, ptr, len);                                                    \
    name = kj::StringPtr(ptr, len);                                                                \
  }

// Convert JS values to/from capnp.
struct JsCapnpConverter {
  kj::Maybe<CapnpTypeWrapperBase&> wrapper;

  capnp::Orphan<capnp::DynamicValue> orphanFromJs(jsg::Lock& js,
      kj::Maybe<capnp::StructSchema::Field> field,
      capnp::Orphanage orphanage,
      capnp::Type type,
      v8::Local<v8::Value> jsValue) {
    return js.withinHandleScope([&]() -> capnp::Orphan<capnp::DynamicValue> {
      switch (type.which()) {
        case capnp::schema::Type::VOID:
          if (jsValue->IsNull()) {
            return capnp::VOID;
          }
          break;
        case capnp::schema::Type::BOOL:
          return jsValue->BooleanValue(js.v8Isolate);
        case capnp::schema::Type::INT8:
          return jsg::check(jsValue->Int32Value(js.v8Context()));
        case capnp::schema::Type::INT16:
          return jsg::check(jsValue->Int32Value(js.v8Context()));
        case capnp::schema::Type::INT32:
          return jsg::check(jsValue->Int32Value(js.v8Context()));
        case capnp::schema::Type::UINT8:
          return jsg::check(jsValue->Uint32Value(js.v8Context()));
        case capnp::schema::Type::UINT16:
          return jsg::check(jsValue->Uint32Value(js.v8Context()));
        case capnp::schema::Type::UINT32:
          return jsg::check(jsValue->Uint32Value(js.v8Context()));
        case capnp::schema::Type::FLOAT32:
          return jsg::check(jsValue->NumberValue(js.v8Context()));
        case capnp::schema::Type::FLOAT64:
          return jsg::check(jsValue->NumberValue(js.v8Context()));
        case capnp::schema::Type::UINT64: {
          if (jsValue->IsNumber()) {
            // js->ToBigInt() doesn't work with Numbers. V8 bug?
            double value = jsg::check(jsValue->NumberValue(js.v8Context()));

            // Casting a double to an integer when the double is out-of-range is UB. `0x1p64` is a
            // C++17 hex double literal with value 2^64. We cannot use UINT64_MAX here because it is
            // not exactly representable as a double, so casting it to double will actually change
            // the value (rounding it up to 2^64). The compiler will rightly produce a warning about
            // this.
            if (value >= 0 && value < 0x1p64 && value == uint64_t(value)) {
              return uint64_t(value);
            }
          } else {
            // Let V8 decide what types can be implicitly cast to BigInt.
            auto bi = jsg::check(jsValue->ToBigInt(js.v8Context()));
            bool lossless;
            uint64_t value = bi->Uint64Value(&lossless);
            if (lossless) {
              return value;
            }
          }
          break;
        }
        case capnp::schema::Type::INT64: {
          // (See comments above for UInt64 case.)
          if (jsValue->IsNumber()) {
            double value = jsg::check(jsValue->NumberValue(js.v8Context()));
            if (value >= -0x1p63 && value < 0x1p63 && value == uint64_t(value)) {
              return uint64_t(value);
            }
          } else {
            auto bi = jsg::check(jsValue->ToBigInt(js.v8Context()));
            bool lossless;
            int64_t value = bi->Int64Value(&lossless);
            if (lossless) {
              return value;
            }
          }
          break;
        }
        case capnp::schema::Type::TEXT: {
          auto str = jsg::check(jsValue->ToString(js.v8Context()));
          capnp::Orphan<capnp::Text> orphan =
              orphanage.newOrphan<capnp::Text>(str->Utf8LengthV2(js.v8Isolate));
          str->WriteUtf8V2(js.v8Isolate, orphan.get().begin(), orphan.get().size());
          return kj::mv(orphan);
        }
        case capnp::schema::Type::DATA:
          if (jsValue->IsArrayBuffer()) {
            auto backing = jsValue.As<v8::ArrayBuffer>()->GetBackingStore();
            return orphanage.newOrphanCopy(capnp::Data::Reader(kj::arrayPtr(
                reinterpret_cast<const kj::byte*>(backing->Data()), backing->ByteLength())));
          } else if (jsValue->IsArrayBufferView()) {
            auto arrayBufferView = jsValue.As<v8::ArrayBufferView>();
            auto backing = arrayBufferView->Buffer()->GetBackingStore();
            kj::ArrayPtr buffer(static_cast<kj::byte*>(backing->Data()), backing->ByteLength());
            auto sliceStart = arrayBufferView->ByteOffset();
            auto sliceEnd = sliceStart + arrayBufferView->ByteLength();
            KJ_ASSERT(buffer.size() >= sliceEnd);
            return orphanage.newOrphanCopy(capnp::Data::Reader(buffer.slice(sliceStart, sliceEnd)));
          }
          break;
        case capnp::schema::Type::LIST: {
          if (jsValue->IsArray()) {
            auto jsArray = jsValue.As<v8::Array>();
            auto schema = type.asList();
            auto elementType = schema.getElementType();
            auto orphan = orphanage.newOrphan(schema, jsArray->Length());
            auto builder = orphan.get();
            if (elementType.isStruct()) {
              // Struct lists can't adopt.
              bool error = false;
              for (uint i: kj::indices(builder)) {
                auto element = jsg::check(jsArray->Get(js.v8Context(), i));
                if (element->IsObject()) {
                  structFromJs(js, builder[i].as<capnp::DynamicStruct>(), element.As<v8::Object>());
                } else {
                  error = true;
                  break;
                }
              }
              if (error) break;
            } else {
              bool isPointerList =
                  builder.as<capnp::AnyList>().getElementSize() == capnp::ElementSize::POINTER;
              for (uint i: kj::indices(builder)) {
                auto jsElement = jsg::check(jsArray->Get(js.v8Context(), i));
                if (isPointerList && (jsElement->IsNull() || jsElement->IsUndefined())) {
                  // Skip null element.
                } else {
                  builder.adopt(i, orphanFromJs(js, field, orphanage, elementType, jsElement));
                }
              }
            }
            return kj::mv(orphan);
          }
          break;
        }
        case capnp::schema::Type::ENUM: {
          auto schema = type.asEnum();
          if (jsValue->IsUint32()) {
            return capnp::DynamicEnum(schema, jsg::check(jsValue->Uint32Value(js.v8Context())));
          }

          STACK_STR(js, name, jsValue, 32);
          KJ_IF_SOME(enumerant, schema.findEnumerantByName(name)) {
            return capnp::DynamicEnum(enumerant);
          }
          break;
        }
        case capnp::schema::Type::STRUCT: {
          if (jsValue->IsObject()) {
            auto schema = type.asStruct();
            auto orphan = orphanage.newOrphan(schema);
            structFromJs(js, orphan.get(), jsValue.As<v8::Object>());
            return kj::mv(orphan);
          }
          break;
        }
        case capnp::schema::Type::INTERFACE: {
          KJ_IF_SOME(wrapper, this->wrapper) {
            auto schema = type.asInterface();
            if (jsValue->IsNull()) {
              auto cap =
                  capnp::Capability::Client(nullptr).castAs<capnp::DynamicCapability>(schema);
              return orphanage.newOrphanCopy(cap);
            } else KJ_IF_SOME(cap, wrapper.tryUnwrapCap(js, js.v8Context(), jsValue)) {
              // We were given a capability type obtained from elsewhere.
              if (cap.getSchema().extends(schema)) {
                return orphanage.newOrphanCopy(cap);
              }
            } else if (jsValue->IsObject()) {
              // We were given a raw object, which we will treat as a server implementation.
              auto cap = IoContext::current().getLocalCapSet().add(
                  kj::heap<CapnpServer>(js, schema, js.v8Ref(jsValue.As<v8::Object>()), wrapper));
              return orphanage.newOrphanCopy(kj::mv(cap));
            }
          }
          break;
        }
        case capnp::schema::Type::ANY_POINTER:
          // TODO(someday): Support this somehow?
          break;
      }

      KJ_IF_SOME(ff, field) {
        JSG_FAIL_REQUIRE(
            TypeError, "Incorrect type for Cap'n Proto field: ", ff.getProto().getName());
      } else {
        JSG_FAIL_REQUIRE(TypeError, "Incorrect type for Cap'n Proto value.");
      }
    });
  }

  void fieldFromJs(jsg::Lock& js,
      capnp::DynamicStruct::Builder builder,
      capnp::StructSchema::Field field,
      v8::Local<v8::Value> jsValue) {
    if (jsValue->IsUndefined()) {
      // Ignore.
      return;
    }
    auto proto = field.getProto();
    switch (proto.which()) {
      case capnp::schema::Field::SLOT: {
        builder.adopt(field,
            orphanFromJs(js, field, capnp::Orphanage::getForMessageContaining(builder),
                field.getType(), jsValue));
        return;
      }

      case capnp::schema::Field::GROUP:
        if (jsValue->IsObject()) {
          structFromJs(
              js, builder.init(field).as<capnp::DynamicStruct>(), jsValue.As<v8::Object>());
        } else {
          JSG_FAIL_REQUIRE(TypeError, "Incorrect type for Cap'n Proto field: ", proto.getName());
        }
        return;
    }

    KJ_FAIL_ASSERT("Unimplemented field type (not slot or group).");
  }

  void structFromJs(
      jsg::Lock& js, capnp::DynamicStruct::Builder builder, v8::Local<v8::Object> jsValue) {
    js.withinHandleScope([&] {
      auto schema = builder.getSchema();
      v8::Local<v8::Array> fieldNames = jsg::check(jsValue->GetOwnPropertyNames(js.v8Context()));
      for (uint i: kj::zeroTo(fieldNames->Length())) {
        auto jsName = jsg::check(fieldNames->Get(js.v8Context(), i));
        STACK_STR(js, fieldName, jsName, 32);
        KJ_IF_SOME(field, schema.findFieldByName(fieldName)) {
          fieldFromJs(js, builder, field, jsg::check(jsValue->Get(js.v8Context(), jsName)));
        } else {
          JSG_FAIL_REQUIRE(TypeError, "No such field in Cap'n Proto struct: ", fieldName);
        }
      }
    });
  }

  void rpcResultsFromJs(jsg::Lock& js,
      capnp::CallContext<capnp::DynamicStruct, capnp::DynamicStruct>& rpcContext,
      v8::Local<v8::Value> jsValue) {
    if (jsValue->IsObject()) {
      structFromJs(js, rpcContext.getResults(), jsValue.As<v8::Object>());
    } else if (jsValue->IsUndefined()) {
      // assume default return
    } else {
      JSG_FAIL_REQUIRE(TypeError, "RPC method server implementation returned a non-object.");
    }
  }

  // ---------------------------------------------------------------------------
  // handle pipelines (as in promise pipelining)
  //
  // In C++, a capnp::RemotePromise<T> represents a combination of a Promise<T::Reader> and a
  // T::Pipeline. The latter is a special object that allows immediately initiating pipeline calls
  // on any capabilities that the response is expected to contain.
  //
  // In JavaScript, we will accomplish something similar by returning a Promise that has been
  // extended with properties representing the pipelined capabilities.

  struct PipelinedCap;
  typedef kj::HashMap<capnp::StructSchema::Field, PipelinedCap> PipelinedCapMap;

  // We return a set of pipelined capabilities on the Promise returned by an RPC call. Later on,
  // that Promise resolves to a response object likely containing the same capabilities again.
  // We don't want the application to have to call `.close()` on both the pipelined version and
  // the final version in order to actually close a capability. So, we need to make sure the final
  // response uses the same CapnpCapability objects that were returned as part of the pipeline.
  // To facilitate this, when we extend the Promise with pipeline properties, we also return a
  // PipelineCapMap which contains all the objects that need to be injected into the final
  // response.
  struct PipelinedCap {
    kj::OneOf<jsg::Ref<CapnpCapability>, PipelinedCapMap> content;
  };

  v8::Local<v8::Object> pipelineStructFieldToJs(jsg::Lock& js,
      capnp::DynamicStruct::Pipeline& pipeline,
      capnp::StructSchema::Field field,
      PipelinedCapMap& capMap) {
    v8::Local<v8::Object> fieldValue = v8::Object::New(js.v8Isolate);
    auto subMap =
        pipelineToJs(js, pipeline.get(field).releaseAs<capnp::DynamicStruct>(), fieldValue);
    if (subMap.size() > 0) {
      // Some capabilities were found in this sub-message, so add it to the map.
      capMap.insert(field, PipelinedCap{kj::mv(subMap)});
    }
    return fieldValue;
  }

  // This function is only useful in the context of RPC, where this->wrapper will always be
  // available.
  PipelinedCapMap pipelineToJs(
      jsg::Lock& js, capnp::DynamicStruct::Pipeline&& pipeline, v8::Local<v8::Object> jsValue) {
    CapnpTypeWrapperBase& wrapper = KJ_REQUIRE_NONNULL(this->wrapper);

    return js.withinHandleScope([&]() -> PipelinedCapMap {
      capnp::StructSchema schema = pipeline.getSchema();

      PipelinedCapMap capMap;

      for (capnp::StructSchema::Field field: schema.getNonUnionFields()) {
        auto proto = field.getProto();
        v8::Local<v8::Value> fieldValue;

        switch (proto.which()) {
          case capnp::schema::Field::SLOT: {
            auto type = field.getType();
            switch (type.which()) {
              case capnp::schema::Type::STRUCT:
                fieldValue = pipelineStructFieldToJs(js, pipeline, field, capMap);
                break;
              case capnp::schema::Type::ANY_POINTER:
                if (type.whichAnyPointerKind() !=
                    capnp::schema::Type::AnyPointer::Unconstrained::CAPABILITY) {
                  continue;
                }
                [[fallthrough]];
              case capnp::schema::Type::INTERFACE: {
                jsg::Ref<CapnpCapability> ref = nullptr;
                fieldValue = wrapper.wrapCap(js, js.v8Context(),
                    pipeline.get(field).releaseAs<capnp::DynamicCapability>(), &ref);
                capMap.insert(field, PipelinedCap{kj::mv(ref)});
                break;
              }
              default:
                continue;
            }
            break;
          }

          case capnp::schema::Field::GROUP:
            fieldValue = pipelineStructFieldToJs(js, pipeline, field, capMap);
            break;

          default:
            continue;
        }

        KJ_ASSERT(!fieldValue.IsEmpty());
        jsg::check(jsValue->Set(
            js.v8Context(), jsg::v8StrIntern(js.v8Isolate, proto.getName()), fieldValue));
      }

      return capMap;
    });
  }

  // ---------------------------------------------------------------------------
  // convert capnp values to JS

  v8::Local<v8::Value> valueToJs(jsg::Lock& js,
      capnp::DynamicValue::Reader value,
      capnp::Type type,
      kj::Maybe<PipelinedCap&> pipelinedCap) {
    // TODO(later): support deserialization outside of RPC, i.e., not requiring a wrapper.
    CapnpTypeWrapperBase& wrapper = KJ_REQUIRE_NONNULL(this->wrapper);

    return js.withinHandleScope([&]() -> v8::Local<v8::Value> {
      switch (value.getType()) {
        case capnp::DynamicValue::UNKNOWN:
          return js.v8Undefined();
        case capnp::DynamicValue::VOID:
          return js.v8Null();
        case capnp::DynamicValue::BOOL:
          return v8::Boolean::New(js.v8Isolate, value.as<bool>());
        case capnp::DynamicValue::INT: {
          if (type.which() == capnp::schema::Type::INT64 ||
              type.which() == capnp::schema::Type::UINT64) {
            return v8::BigInt::New(js.v8Isolate, value.as<int64_t>());
          } else {
            return v8::Integer::New(js.v8Isolate, value.as<int32_t>());
          }
        }
        case capnp::DynamicValue::UINT: {
          if (type.which() == capnp::schema::Type::INT64 ||
              type.which() == capnp::schema::Type::UINT64) {
            return v8::BigInt::NewFromUnsigned(js.v8Isolate, value.as<uint64_t>());
          } else {
            return v8::Integer::NewFromUnsigned(js.v8Isolate, value.as<uint32_t>());
          }
        }
        case capnp::DynamicValue::FLOAT:
          return v8::Number::New(js.v8Isolate, value.as<double>());
        case capnp::DynamicValue::TEXT:
          return jsg::v8Str(js.v8Isolate, value.as<capnp::Text>());
        case capnp::DynamicValue::DATA: {
          capnp::Data::Reader data = value.as<capnp::Data>();

          // In theory we could avoid a copy if we kept the response message in memory, but we
          // probably don't want to do that.
          auto result = jsg::check(v8::ArrayBuffer::MaybeNew(js.v8Isolate, data.size()));
          memcpy(result->GetBackingStore()->Data(), data.begin(), data.size());

          return result;
        }
        case capnp::DynamicValue::LIST: {
          capnp::DynamicList::Reader list = value.as<capnp::DynamicList>();
          auto elementType = list.getSchema().getElementType();
          auto indices = kj::indices(list);
          KJ_STACK_ARRAY(v8::Local<v8::Value>, items, indices.size(), 100, 100);
          for (uint i: indices) {
            items[i] = valueToJs(js, list[i], elementType, kj::none);
          }
          return v8::Array::New(js.v8Isolate, items.begin(), items.size());
        }
        case capnp::DynamicValue::ENUM: {
          auto enumValue = value.as<capnp::DynamicEnum>();
          KJ_IF_SOME(enumerant, enumValue.getEnumerant()) {
            return jsg::v8StrIntern(js.v8Isolate, enumerant.getProto().getName());
          } else {
            return v8::Integer::NewFromUnsigned(js.v8Isolate, enumValue.getRaw());
          }
        }
        case capnp::DynamicValue::STRUCT: {
          auto capMap = pipelinedCap.map([](PipelinedCap& pc) -> PipelinedCapMap& {
            // If we had a PipelinedCap for a struct field, it must be a PipelinedCapMap.
            return pc.content.get<PipelinedCapMap>();
          });

          capnp::DynamicStruct::Reader reader = value.as<capnp::DynamicStruct>();
          auto object = v8::Object::New(js.v8Isolate);
          KJ_IF_SOME(field, reader.which()) {
            fieldToJs(js, object, reader, field, capMap);
          }

          for (auto field: reader.getSchema().getNonUnionFields()) {
            if (reader.has(field)) {
              fieldToJs(js, object, reader, field, capMap);
            }
          }
          return object;
        }
        case capnp::DynamicValue::CAPABILITY:
          KJ_IF_SOME(p, pipelinedCap) {
            // Use the same CapnpCapability object that we returned earlier for promise pipelining.
            // Note: We know the JS wrapper exists because CapnpCapability objects are always created
            //   by CapnpTypeWrapper::wrap() and immediately have a wrapper added.
            return KJ_ASSERT_NONNULL(p.content.get<jsg::Ref<CapnpCapability>>().tryGetHandle(js));
          } else {
            return wrapper.wrapCap(js, js.v8Context(), value.as<capnp::DynamicCapability>());
          }
        case capnp::DynamicValue::ANY_POINTER:
          return js.v8Null();
      }

      KJ_FAIL_ASSERT("Unimplemented DynamicValue type.");
    });
  }

  void fieldToJs(jsg::Lock& js,
      v8::Local<v8::Object> object,
      capnp::DynamicStruct::Reader reader,
      capnp::StructSchema::Field field,
      kj::Maybe<PipelinedCapMap&> capMap) {
    js.withinHandleScope([&] {
      kj::Maybe<PipelinedCap&> pipelinedCap;
      KJ_IF_SOME(m, capMap) {
        pipelinedCap = m.find(field);
      }

      auto proto = field.getProto();
      v8::Local<v8::Value> fieldValue;
      switch (proto.which()) {
        case capnp::schema::Field::SLOT:
          fieldValue = valueToJs(js, reader.get(field), field.getType(), pipelinedCap);
          break;
        case capnp::schema::Field::GROUP:
          fieldValue = valueToJs(js, reader.get(field), field.getType(), pipelinedCap);
          break;
      }

      JSG_REQUIRE(
          !fieldValue.IsEmpty(), TypeError, "Unimplemented field type (not slot or group).");

      jsg::check(
          object->Set(js.v8Context(), jsg::v8StrIntern(js.v8Isolate, proto.getName()), fieldValue));
    });
  }
};

// =======================================================================================

void fillCapnpFieldFromJs(jsg::Lock& js,
    capnp::DynamicStruct::Builder builder,
    capnp::StructSchema::Field field,
    v8::Local<v8::Value> jsValue) {
  JsCapnpConverter converter;
  converter.fieldFromJs(js, builder, field, jsValue);
}

capnp::Orphan<capnp::DynamicValue> capnpValueFromJs(
    jsg::Lock& js, capnp::Orphanage orphanage, capnp::Type type, v8::Local<v8::Value> jsValue) {
  JsCapnpConverter converter;
  return converter.orphanFromJs(js, kj::none, orphanage, type, jsValue);
}

// =======================================================================================

CapnpServer::CapnpServer(jsg::Lock& js,
    capnp::InterfaceSchema schema,
    jsg::V8Ref<v8::Object> objectParam,
    CapnpTypeWrapperBase& wrapper)
    : capnp::DynamicCapability::Server(schema),
      ioContext(IoContext::current().getWeakRef()),
      object(kj::mv(objectParam)),
      closeMethod(getCloseMethod(js)),
      wrapper(wrapper) {}

kj::Maybe<jsg::V8Ref<v8::Function>> CapnpServer::getCloseMethod(jsg::Lock& js) {
  auto handle = object.getHandle(js);
  auto methodHandle =
      jsg::check(handle->Get(js.v8Context(), jsg::v8StrIntern(js.v8Isolate, "close")));
  if (methodHandle->IsFunction()) {
    return js.v8Ref(methodHandle.As<v8::Function>());
  } else {
    return kj::none;
  }
}

CapnpServer::~CapnpServer() noexcept(false) {
  KJ_IF_SOME(c, closeMethod) {
    ioContext->runIfAlive([&](IoContext& rc) {
      rc.addTask(
          rc.run([object = kj::mv(object), closeMethod = kj::mv(c)](Worker::Lock& lock) mutable {
        auto handle = object.getHandle(lock);
        auto methodHandle = closeMethod.getHandle(lock);
        if (methodHandle->IsFunction()) {
          jsg::check(methodHandle.As<v8::Function>()->Call(lock.getContext(), handle, 0, nullptr));
        }
      }));
    });
  }
}

kj::Promise<void> CapnpServer::call(capnp::InterfaceSchema::Method method,
    capnp::CallContext<capnp::DynamicStruct, capnp::DynamicStruct> rpcContext) {
  kj::Promise<void> result = nullptr;

  bool live = ioContext->runIfAlive([&](IoContext& rc) {
    result =
        rc.run([this, method, rpcContext, &rc](Worker::Lock& lock) mutable -> kj::Promise<void> {
      jsg::Lock& js = lock;
      auto handle = object.getHandle(js);
      auto methodName = method.getProto().getName();
      auto methodHandle =
          jsg::check(handle->Get(lock.getContext(), jsg::v8StrIntern(js.v8Isolate, methodName)));

      if (!methodHandle->IsFunction()) {
        KJ_UNIMPLEMENTED(kj::str("jsg.Error: RPC method not implemented: ", methodName));
      }

      JsCapnpConverter converter{wrapper};
      auto params = rpcContext.getParams();
      auto jsParams = converter.valueToJs(js, params, params.getSchema(), kj::none);
      rpcContext.releaseParams();

      auto result = jsg::check(
          methodHandle.As<v8::Function>()->Call(lock.getContext(), handle, 1, &jsParams));
      KJ_IF_SOME(promise, wrapper.tryUnwrapPromise(lock, lock.getContext(), result)) {
        return rc.awaitJs(js,
            promise.then(
                js, rc.addFunctor([this, rpcContext](jsg::Lock& js, jsg::Value result) mutable {
          JsCapnpConverter converter{wrapper};
          converter.rpcResultsFromJs(js, rpcContext, result.getHandle(js));
        })));

      } else {
        converter.rpcResultsFromJs(js, rpcContext, result);
        return kj::READY_NOW;
      }
    });
  });

  if (live) {
    return result;
  } else {
    return KJ_EXCEPTION(DISCONNECTED, "jsg.Error: Called to event context that is no longer live.");
  }
}

// =======================================================================================

CapnpCapability::CapnpCapability(capnp::DynamicCapability::Client client)
    : schema(client.getSchema()),
      client(IoContext::current().addObject(kj::heap(kj::mv(client)))) {}

CapnpCapability::~CapnpCapability() noexcept(false) {
  KJ_IF_SOME(c, client) {
    // The client was not explicitly close()ed and instead waited for GC. There are two problems
    // with this:
    // 1. It's rude to force the remote peer to wait until the lazy garbage collector gets around
    //    to collecting the object before we let the peer know that it can clean up its end. Our
    //    GC is sociopathic, it decides when to collect based purely on its own memory pressure
    //    and has no idea what memory pressure the peer might be feeling, so likely won't make
    //    empathetic choices about when to collect.
    // 2. We generally do not want to allow an application to observe its own garbage collection
    //    behavior, as this may reveal side channels. The capability could be a loopback into
    //    this very isolate, in which case closing it now would immediately call back into the
    //    server's close() method, notifying the application of its own GC. We need to prevent that.

    // To solve #2, we defer destruction of the object until the end of the IoContext.
    kj::mv(c).deferGcToContext();

    // In preview, let's try to warn the developer about the problem.
    //
    // TODO(cleanup): Instead of logging this warning at GC time, it would be better if we logged
    //   it at the time that the client is destroyed, i.e. when the IoContext is torn down,
    //   which is usually sooner (and more deterministic). But logging a warning during
    //   IoContext tear-down is problematic since logWarningOnce() is a method on
    //   IoContext...
    if (IoContext::hasCurrent()) {
      IoContext::current().logWarningOnce(
          kj::str("A Cap'n Proto capability of type ", schema.getShortDisplayName(),
              " was not closed properly. You must call close() on all capabilities in order to "
              "let the other side know that you are no longer using them. You cannot rely on "
              "the garbage collector for this because it may take arbitrarily long before actually "
              "collecting unreachable objects."));
    }
  }
}

v8::Local<v8::Value> CapnpCapability::call(jsg::Lock& js,
    capnp::InterfaceSchema::Method method,
    v8::Local<v8::Value> params,
    CapnpTypeWrapperBase& wrapper) {
  auto& ioContext = IoContext::current();
  auto req = getClient(js, wrapper).newRequest(method);
  JsCapnpConverter converter{wrapper};
  if (params->IsObject()) {
    converter.structFromJs(js, req, params.As<v8::Object>());
  } else if (params->IsUndefined()) {
    // leave params all-default
  } else {
    JSG_FAIL_REQUIRE(TypeError, "Argument to a capnp RPC call must be an object.");
  }
  if (method.isStreaming()) {
    // Note: We know the JS wrapper exists for JSG_THIS because CapnpCapability objects are always
    //   created by CapnpTypeWrapper::wrap() and immediately have a wrapper added.
    return wrapper.wrapPromise(js, js.v8Context(), KJ_ASSERT_NONNULL(JSG_THIS.tryGetHandle(js)),
        ioContext.awaitIo(
            js, req.sendStreaming(), [](jsg::Lock& js) { return js.v8Ref(js.v8Undefined()); }));
  } else {
    // The RPC promise is actually both a promise and a pipeline.
    auto rpcPromise = req.send();

    auto pipelinedCapHolder = kj::heap<JsCapnpConverter::PipelinedCap>();
    auto& pipelinedCapRef = *pipelinedCapHolder;

    // We'll consume the promise itself to handle converting the response.
    // Note: We know the JS wrapper exists for JSG_THIS because CapnpCapability objects are always
    //   created by CapnpTypeWrapper::wrap() and immediately have a wrapper added.
    auto responsePromise =
        kj::Promise<capnp::Response<capnp::DynamicStruct>>(kj::mv(rpcPromise))
            .catch_([](kj::Exception&& ex) -> kj::Promise<capnp::Response<capnp::DynamicStruct>> {
      auto errorType = jsg::tunneledErrorType(ex.getDescription());
      if (!errorType.isJsgError) {
        // Wrap any non-JS exceptions as JS errors
        auto newDescription =
            kj::str("remote." JSG_EXCEPTION(Error) ": capnp RPC exception: "_kj, errorType.message);
        ex.setDescription(kj::mv(newDescription));
      }
      return kj::mv(ex);
    });
    auto result =
        wrapper.wrapPromise(js, js.v8Context(), KJ_ASSERT_NONNULL(JSG_THIS.tryGetHandle(js)),
            ioContext.awaitIo(js, kj::mv(responsePromise),
                [&wrapper, pipelinedCapHolder = kj::mv(pipelinedCapHolder)](
                    jsg::Lock& js, capnp::Response<capnp::DynamicStruct> resp) mutable {
      JsCapnpConverter converter{wrapper};
      return js.v8Ref(converter.valueToJs(js, resp, resp.getSchema(), *pipelinedCapHolder));
    }));

    // Now we take the pipeline part of `rpcPromise` and merge it into the V8 promise object, by
    // adding fields representing the pipelined struct.
    KJ_ASSERT(result->IsPromise());
    pipelinedCapRef.content =
        converter.pipelineToJs(js, kj::mv(rpcPromise), result.As<v8::Promise>());

    return result;
  }
}

void CapnpCapability::close() {
  KJ_IF_SOME(c, client) {
    // Verify we're in the correct IoContext. This will throw otherwise.
    *c;
  }
  client = kj::none;
}

jsg::Promise<kj::Maybe<jsg::V8Ref<v8::Object>>> CapnpCapability::unwrap(jsg::Lock& js) {
  // We need to allocate a heap copy of the `Client` so that if this capability is closed while
  // the promise is still outstanding, the client isn't destroyed, which would otherwise cause
  // UAF in the getLocalServer() implementation.
  auto capHolder = kj::heap(*JSG_REQUIRE_NONNULL(client, Error, "Capability has been closed."));
  auto& ioContext = IoContext::current();
  auto promise = ioContext.getLocalCapSet().getLocalServer(*capHolder);

  return ioContext.awaitIo(js, kj::mv(promise),
      [capHolder = kj::mv(capHolder)](
          jsg::Lock& js, kj::Maybe<capnp::DynamicCapability::Server&> server) {
    return server.map([&](capnp::DynamicCapability::Server& s) {
      return kj::downcast<CapnpServer>(s).object.addRef(js);
    });
  });
}

capnp::DynamicCapability::Client CapnpCapability::getClient(
    jsg::Lock&, CapnpTypeWrapperBase& wrapper) {
  return *JSG_REQUIRE_NONNULL(client, Error, "Capability has been closed.");
}

}  // namespace workerd::api
