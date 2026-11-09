// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Types describing a V8 startup snapshot and how an isolate relates to one. Only high-level code
// that creates isolates (jsg::IsolateBase and its callers) needs to include this file.

#include <cppgc/persistent.h>
#include <v8-object.h>
#include <v8-sandbox.h>
#include <v8-snapshot.h>

#include <kj/array.h>
#include <kj/function.h>
#include <kj/map.h>
#include <kj/one-of.h>
#include <kj/refcount.h>
#include <kj/string.h>
#include <kj/vector.h>

namespace workerd::jsg {

// Index of the bindings scope (the `env` object).
constexpr size_t BINDINGS_SCOPE_SNAPSHOT_INDEX = 0;

// Index of the main module's namespace object.
constexpr size_t MAIN_MODULE_NAMESPACE_SNAPSHOT_INDEX = 1;

class HeapTracer;
class Wrappable;

// Everything needed to create a new isolate from a V8 startup snapshot:
// * `blob` is the serialized snapshot data, allocated with `new[]` by v8::SnapshotCreator.
// * `externalReferences` is the list of addresses of all C++ functions and objects referenced
//   from the snapshot. The producer and the consumer of a snapshot must register exactly the
//   same references, in the same order, or deserialization will fail.
// * `externalReferenceCursor` is the next free slot in `externalReferences`.
// * `constructorTemplateIndices` is the snapshot index of every resource type's
//   constructor-template slot (memoizedConstructor, then contextConstructor, per JSG_RESOURCE
//   type), in iterateResourceTypeTemplates order. The enumeration order is
//   fixed at compile time and the producer and the consumer of a snapshot are the same binary,
//   the same invariant `externalReferences` relies on.
// * `kNoConstructorTemplate` marks slots that were empty when the snapshot was prepared.
// * `liveWrappables` is the native side of the wrappers serialized into `blob`, indexed by the
//   payload the API-wrapper serializer wrote for each of them. Each entry carries the
//   CppHeapPointerTag the original's wrapper had and, for a stateful type, a snapshotClone() of
//   the zygote's original. A stateless type (Object::jsgSnapshotStateless) is recorded by its tag
//   alone. Slots whose index never reached the blob hold no transfer.
// * `bootstrapStateIndex` is the context-level SnapshotCreator::AddData index of the per-isolate
//   bootstrap's JS-held state (require(), primordials, the compatFlags, autogates and utils
//   objects, the require() cache), bundled into one object and pinned by the zygote right before
//   CreateBlob. A worker started from the snapshot rebuilds its C++ BootstrapState from it instead
//   of running the bootstrap scripts again. None when the zygote ran without the per-isolate
//   bootstrap.
// * `restoreTimeGlobalsIndex` is the context-level AddData index of the record of what the
//   zygote's top level left of the properties V8 adds to a context only on restore
//   (see setup.h), pinned right before CreateBlob. None until the zygote has run
//   removeRestoreTimeGlobalsBeforeSnapshot().
// * `moduleRecords` are the module-registry handles pinned into the snapshot at
//   PREPARE_SNAPSHOT, one (specifier, type, kind) -> context-level AddData index per handle,
//   so the loading worker can build its entries around the baked module instances instead of
//   compiling them again (preserving identity between the baked main-module graph and runtime
//   import()/require()). Field types are raw ints to avoid coupling this header to modules.h:
//   `moduleType` is a jsg::ModuleRegistry::Type (capnp ModuleType), `handleKind` a
//   jsg::ModuleRegistry::SnapshotHandleKind.
// * `listenerRecords` are the global scope's event listeners the zygote's top level registered
//   via addEventListener(), in per-type registration order: for each, its type, its `once` and
//   `passive` options, and the context-level AddData indices of its identity object and of the
//   callback and receiver its registration derived. The fresh ServiceWorkerGlobalScope built by
//   newContext() at load discards the zygote's listener map, so the loading worker re-registers
//   each listener in C++ (EventTarget::restoreSnapshotListener()), without running JavaScript.
struct SnapshotArtifact: public kj::AtomicRefcounted {
  v8::StartupData blob{nullptr, 0};
  kj::Array<intptr_t> externalReferences;
  size_t externalReferenceCursor = 0;

  static constexpr size_t kNoConstructorTemplate = kj::maxValue;
  kj::Vector<size_t> constructorTemplateIndices;

  struct LiveWrappable {
    // A type recorded by its tag alone.
    struct Stateless {};
    // Empty (== nullptr) once dropUnreachableWrappables() dropped the slot.
    using Transfer = kj::OneOf<Stateless, kj::Own<Wrappable>>;
    Transfer transfer;
    v8::CppHeapPointerTag tag;
  };
  kj::Vector<LiveWrappable> liveWrappables;

  kj::Maybe<size_t> bootstrapStateIndex;

  kj::Maybe<size_t> restoreTimeGlobalsIndex;

  struct ModuleRecord {
    kj::String specifier;
    uint8_t moduleType;
    uint8_t handleKind;
    size_t dataIndex;
  };
  kj::Vector<ModuleRecord> moduleRecords;

  // A listener the zygote's top level registered on the context global, re-registered as it was
  // on load: the identity removeEventListener() matches, and the callback and receiver its
  // registration derived (a handleEvent object's method is read once, at registration).
  struct ListenerRecord {
    kj::String type;
    size_t identityIndex;
    size_t callbackIndex;
    size_t receiverIndex;
    bool once;
    bool passive;
  };
  kj::Vector<ListenerRecord> listenerRecords;

  ~SnapshotArtifact() noexcept(false) {
    // v8::SnapshotCreator::CreateBlob() allocates the data with `new[]` and hands over ownership.
    delete[] blob.data;
  }

  kj::Arc<SnapshotArtifact> addRef() const {
    return addRefToThis();
  }
};

// SerializeInternalFieldsCallback registered with the snapshot's default context
// (IsolateBase::setSnapshotDefaultContext). V8 runs it inside CreateBlob() for every embedder
// field that does not hold a heap object (aligned pointers and empty slots alike) the context
// reaches. `data` is a uint32_t counter of non-null aligned-pointer fields at the wrong index or
// with an unknown marker. V8 is built without exceptions, so the callback only increments the
// counter and IsolateBase::createSnapshotBlob() raises it after CreateBlob() returns.
v8::StartupData serializeInternalFields(v8::Local<v8::Object> holder, int index, void* data);

// Metadata needed to serialize live wrappable instances.
// First, every live Wrappable is recorded in the snapshot artifact: by its tag alone for a
// stateless type, as a clone otherwise. Then a map from cppgc shim pointer to index within the
// snapshot artifact is built, so that the connection between the C++ Wrappable, its JS wrapper and
// the cppgc shim can be restored later.
struct WrappablesSerializeMetadata {
  struct Entry {
    // Slot in SnapshotArtifact::liveWrappables, or none for a type that is neither stateless nor
    // has snapshotClone(), and for the context global (rebuilt by newContext() on load, never
    // transferred).
    kj::Maybe<size_t> index;
    v8::CppHeapPointerTag tag;

    // Named by the clonability gate. Captured while the original was alive.
    kj::String typeName;

    // The serialized payload: `index`, at an address that stays valid while V8 copies it.
    size_t payload = 0;

    // Whether the serializer emitted `index`, i.e. the wrapper was reachable at CreateBlob().
    bool serialized = false;

    // The shim itself, rooted so that it stays allocated for as long as this metadata lives: the
    // entry's key is its address, and a shim swept by a GC could otherwise be reallocated at the
    // same address for a wrapper created afterwards, which the serializer would then report
    // under this entry.
    cppgc::Persistent<v8::Object::Wrappable> shim;
  };

  // Keyed by the v8::Object::Wrappable base of the CppgcShim, the pointer V8 reports to the
  // API-wrapper serializer. Each entry roots its shim; see Entry::shim.
  kj::HashMap<const void*, Entry> entries;

  // Findings the callback cannot raise itself (V8 is built without exceptions); checked by
  // dropUnreachableWrappables().
  kj::Vector<kj::String> unclonable;
  uint32_t unknownWrappersCount = 0;

  // Record the native side of every live Wrappable while it still exists into the snapshot
  // artifact (by tag alone when `isStateless(tag)`, as a snapshotClone() otherwise), and build a
  // map from each one's cppgc pointer to its index in the snapshot artifact to serialize the
  // connection: JS Wrapper -> cppgc shim -> C++ Wrappable.
  void recordLiveWrappables(HeapTracer& tracer,
      SnapshotArtifact& artifact,
      kj::FunctionParam<bool(v8::CppHeapPointerTag)> isStateless);

  // Raises the findings the serializer callback accumulated, then drops the
  // transfers of wrappers whose index never reached the blob.
  void dropUnreachableWrappables(SnapshotArtifact& artifact);
};

// SerializeAPIWrapperCallback registered with the snapshot's default context
// (IsolateBase::setSnapshotDefaultContext). V8 runs it inside CreateBlob() for every API wrapper
// object the context reaches. `data` is the WrappablesSerializeMetadata. Must not throw: findings
// are recorded in the metadata.
v8::StartupData serializeApiWrapperCallback(
    v8::Local<v8::Object> holder, void* cppHeapPointer, void* data);

// Deserialization metadata needed to:
// (1) Preallocate all cppgc shims holding C++ Wrappables. This has to happen before the context
//     is deserialized because allocations are not supported inside callbacks.
// (2) Recreate the link between each C++ Wrappable, its preallocated cppgc shim and its JS wrapper
//     inside the deserialization callback.
struct WrappableDeserializeMetadata {
  // The shim pre-allocated for this slot's instance, or empty for a dropped slot.
  struct Slot {
    cppgc::Persistent<v8::Object::Wrappable> shim;
    bool linked = false;
  };

  v8::Isolate* isolate = nullptr;

  // Parallel to SnapshotArtifact::liveWrappables.
  kj::Array<Slot> slots;

  // Findings the callback cannot raise itself.
  uint32_t badPayloadsCount = 0;     // API-wrapper payload with a bad size, index or slot
  uint32_t duplicateLinksCount = 0;  // one slot referenced by more than one holder

  // Before creating a context: pre-allocates the CppgcShim that will back each restored wrapper,
  // so that the deserializer callback only needs to link holder and shim. `instances` is parallel
  // to `artifact.liveWrappables`: the object each slot's restored wrapper is attached to (see
  // IsolateBase::prepareWrappableDeserializeMetadata()), null for a dropped slot.
  void preallocateCppgcShims(v8::Isolate* isolate,
      HeapTracer& tracer,
      const SnapshotArtifact& artifact,
      kj::Array<kj::Own<Wrappable>> instances);

  // After creating a context: verifies that every serialized slot was linked exactly once and that
  // the callback reported nothing else, then releases the roots that kept unlinked shims alive.
  void releaseSlotsWithChecks();
};

// DeserializeInternalFieldsCallback for Context::New() from a startup snapshot. Restores the
// workerd marker in WRAPPABLE_TAG_FIELD_INDEX of each deserialized wrapper from the marker value
// the serializer emitted. `data` is a uint32_t counter of payloads at the wrong index or with an
// unknown marker; the caller raises it after Context::New() returns.
void deserializeInternalFields(
    v8::Local<v8::Object> holder, int index, v8::StartupData payload, void* data);

// DeserializeAPIWrapperCallback for Context::New() from a startup snapshot. The payload is the
// liveWrappables index the serializer emitted; links the holder to the shim pre-allocated for
// that slot. Runs under V8's DisallowGarbageCollection, so it allocates nothing. `data` is the
// WrappableDeserializeMetadata.
void deserializeApiWrapperCallback(
    v8::Local<v8::Object> holder, v8::StartupData payload, void* data);

// Different views for snapshot artifacts:
// * MutableSnapshot: a zygote isolate built via v8::SnapshotCreator; holds an owning ref to
//   the artifact and fills it in IsolateBase::createSnapshotBlob().
// * FinalizedSnapshot: an isolate that boots from a previously produced blob; the Arc
//   keeps the artifact alive for the isolate's entire life.
struct MutableSnapshot {
  kj::Own<SnapshotArtifact> artifact;
};
struct FinalizedSnapshot {
  kj::Arc<SnapshotArtifact> artifact;
};
using SnapshotConfig = kj::OneOf<MutableSnapshot, FinalizedSnapshot>;

}  // namespace workerd::jsg
