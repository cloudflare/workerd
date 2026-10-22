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
// * `liveWrappables` is the native backing state of the wrappers serialized into `blob`, indexed
//   by the uint32 payload the API-wrapper serializer wrote for each of them. Each entry holds a
//   snapshotClone() of the zygote's original plus the CppHeapPointerTag the original's wrapper carried.
//   A worker started from this artifact clones the stored clone once more per restored wrapper
//   and attaches it with the same tag. Slots whose index never reached the blob hold a null clone.
// * `bootstrapStateIndex` is the context-level SnapshotCreator::AddData index of the per-isolate
//   bootstrap's JS-held state (require(), primordials, the compatFlags, autogates and utils
//   objects, the require() cache), bundled into one object and pinned by the zygote right before
//   CreateBlob. A worker started from the snapshot rebuilds its C++ BootstrapState from it instead
//   of running the bootstrap scripts again. None when the zygote ran without the per-isolate
//   bootstrap.
struct SnapshotArtifact: public kj::AtomicRefcounted {
  v8::StartupData blob{nullptr, 0};
  kj::Array<intptr_t> externalReferences;
  size_t externalReferenceCursor = 0;

  static constexpr uint32_t kNoConstructorTemplate = kj::maxValue;
  kj::Vector<uint32_t> constructorTemplateIndices;

  struct LiveWrappable {
    kj::Own<Wrappable> clone;
    v8::CppHeapPointerTag tag;
  };
  kj::Vector<LiveWrappable> liveWrappables;

  kj::Maybe<uint32_t> bootstrapStateIndex;

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
// First, every live Wrappable is cloned and the clones are placed in the snapshot artifact.
// Then a map from cppgc shim pointer to index within the snapshot artifact is built, so that the
// connection between the C++ Wrappable, its JS wrapper and the cppgc shim can be restored later.
struct WrappablesSerializeMetadata {
  struct Entry {
    // Slot in SnapshotArtifact::liveWrappables, or none for a type without snapshotClone() and
    // for the context global (rebuilt by newContext() on load, never transferred).
    kj::Maybe<uint32_t> index;
    v8::CppHeapPointerTag tag;

    // Named by the clonability gate. Captured while the original was alive.
    kj::String typeName;

    // The serialized payload: `index`, at an address that stays valid while V8 copies it.
    uint32_t payload = 0;

    // Whether the serializer emitted `index`, i.e. the wrapper was reachable at CreateBlob().
    bool serialized = false;
  };

  // Keyed by the v8::Object::Wrappable base of the CppgcShim.
  kj::HashMap<const void*, Entry> entries;

  // Findings the callback cannot raise itself (V8 is built without exceptions); checked by
  // verify().
  kj::Vector<kj::String> unclonable;
  uint32_t unknownWrappersCount = 0;

  // Clone the native side of every live Wrappable while it still exists into the snapshot artifact,
  // and build a map from each one's cppgc pointer to its index in the snapshot artifact to serialize
  // the connection: JS Wrapper -> cppgc shim -> C++ Wrappable.
  void cloneAndRecordLiveWrappables(HeapTracer& tracer, SnapshotArtifact& artifact);

  // Raises the findings the serializer callback accumulated, then drops the
  // clones of wrappers whose index never reached the blob.
  void verify(SnapshotArtifact& artifact);
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
  // The shim pre-allocated for this slot's clone, or empty for a slot whose clone is null.
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

  // Before creating a context: clones every transferable wrapper's stored clone once more and
  // pre-allocates the CppgcShim that will back it, so that the deserializer callback only needs
  // to link holder and shim.
  void preallocateCppgcShims(
      v8::Isolate* isolate, HeapTracer& tracer, const SnapshotArtifact& artifact);

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
