// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "snapshot.h"

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/wrappable.h>

#include <v8-isolate.h>

#include <kj/debug.h>

#include <cstring>

namespace workerd::jsg {

SnapshotArtifact::~SnapshotArtifact() noexcept(false) {
  // v8::SnapshotCreator::CreateBlob() allocates the data with `new[]` and hands over ownership.
  delete[] blob.data;
}

constexpr size_t internalFieldTagSizeInBytes = sizeof(Wrappable::WORKERD_WRAPPABLE_TAG);

// The zygote's half of the live-wrappable transfer, under the isolate lock and before
// destroyLiveWrappableInstances(). A snapshotClone() that throws propagates out of
// IsolateBase::createSnapshotBlob() and fails the zygote; the Worker then starts without a
// snapshot.
void WrappablesSerializeMetadata::recordLiveWrappables(HeapTracer& tracer,
    SnapshotArtifact& artifact,
    kj::FunctionParam<bool(v8::CppHeapPointerTag)> isStateless) {
  using LiveWrappable = SnapshotArtifact::LiveWrappable;
  // The slot of each recorded object, and what each one references, resolved once every live
  // wrapper is recorded.
  kj::HashMap<const Wrappable*, size_t> slotOf;
  struct PendingReferences {
    size_t slot;
    kj::Array<const Wrappable*> references;
  };
  kj::Vector<PendingReferences> pending;
  tracer.forEachLiveWrapper([&artifact, &isStateless, &slotOf, &pending, this](Wrappable& wrappable,
                                v8::Object::Wrappable& shim, v8::CppHeapPointerTag tag) {
    kj::Maybe<size_t> index;
    if (static_cast<uint16_t>(tag) != kContextGlobalWrappableTag) {
      LiveWrappable::Transfer transfer;
      if (isStateless(tag)) {
        transfer = LiveWrappable::Stateless{};
      } else KJ_IF_SOME(clone, wrappable.snapshotClone()) {
        // snapshotClone() is virtual, so a subclass that does not override it inherits its base's
        // clone. Attaching that to the subclass's restored wrapper would put a base instance under
        // the subclass's tag and abort the first unwrap in the restored worker. Every
        // JSG_RESOURCE_TYPE names its own type, so the names differ exactly when the clone is of
        // another type.
        KJ_REQUIRE(clone->jsgGetMemoryName() == wrappable.jsgGetMemoryName(),
            "startup snapshot: snapshotClone() returned an instance of another type; a subclass "
            "must override it",
            wrappable.jsgGetMemoryName(), clone->jsgGetMemoryName());
        transfer = kj::mv(clone);
      }
      if (transfer != nullptr) {
        auto slot = artifact.liveWrappables.size();
        index = slot;
        artifact.liveWrappables.add(LiveWrappable{kj::mv(transfer), tag, nullptr});
        slotOf.insert(&wrappable, slot);
        auto references = wrappable.snapshotReferences();
        if (references.size() > 0) {
          pending.add(PendingReferences{slot, kj::mv(references)});
        }
      }
    }

    // Recorded even without a clone. This list holds every wrapper not yet swept, including
    // ones JS no longer reaches, and the GC inside CreateBlob() collects those before the
    // serializer runs, so only the serializer callback knows whether a wrapper without a clone
    // actually reaches the blob.
    entries.insert(&shim,
        Entry{.index = index,
          .tag = tag,
          .typeName = kj::str(wrappable.jsgGetMemoryName()),
          .shim = cppgc::Persistent<v8::Object::Wrappable>(&shim)});
  });
  for (auto& owner: pending) {
    auto slots = kj::heapArrayBuilder<kj::Maybe<size_t>>(owner.references.size());
    for (auto reference: owner.references) {
      kj::Maybe<size_t> slot;
      KJ_IF_SOME(found, slotOf.find(reference)) {
        slot = found;
      }
      slots.add(slot);
    }
    artifact.liveWrappables[owner.slot].references = slots.finish();
  }
}

void WrappablesSerializeMetadata::checkCallbackFindings() {
  KJ_REQUIRE(unknownWrappersCount == 0,
      "PREPARE_SNAPSHOT: wrapper objects reachable from the context carry CppHeap pointers that "
      "were not recorded before the wrappers were destroyed (wrappers created after "
      "destroyLiveWrappableInstances()?)",
      unknownWrappersCount);

  KJ_REQUIRE(unclonable.empty(),
      "PREPARE_SNAPSHOT: live wrapper types that neither set jsgSnapshotStateless nor implement "
      "snapshotClone() were reachable from the context; these types cannot be transferred "
      "through the startup snapshot",
      kj::strArray(unclonable, ", "));
}

void WrappablesSerializeMetadata::dropUnreachableWrappables(SnapshotArtifact& artifact) {
  // The slots stay so the serialized indices remain valid.
  for (auto& entry: entries) {
    KJ_IF_SOME(index, entry.value.index) {
      if (!entry.value.serialized) {
        artifact.liveWrappables[index].transfer = SnapshotArtifact::LiveWrappable::Transfer();
      }
    }
  }
}

// The restored worker's half of the live-wrappable transfer, before Context::New(). The artifact
// is shared by every worker started from it, so a slot's transfer is only the model: the object
// attached to the restored wrapper is this worker's own instance. Nothing on this path catches an
// exception from a recreate function or a snapshotClone(): it propagates out of newContext() and
// the Worker fails to start, which is why the contracts on Object::jsgSnapshotStateless and
// Wrappable::snapshotClone() forbid throwing.
void WrappableDeserializeMetadata::preallocateCppgcShims(Lock& js,
    HeapTracer& tracer,
    const SnapshotArtifact& artifact,
    kj::FunctionParam<kj::Maybe<SnapshotRecreateFn>(v8::CppHeapPointerTag)> recreateFnFor) {
  isolate = js.v8Isolate;
  slots = kj::heapArray<Slot>(artifact.liveWrappables.size());
  auto instancesBuilder = kj::heapArrayBuilder<Wrappable*>(artifact.liveWrappables.size());
  for (auto i: kj::indices(artifact.liveWrappables)) {
    auto& stored = artifact.liveWrappables[i];
    if (stored.transfer == nullptr) {
      instancesBuilder.add(nullptr);
      continue;
    }
    kj::Own<Wrappable> instance;
    KJ_SWITCH_ONEOF(stored.transfer) {
      KJ_CASE_ONEOF(stateless, SnapshotArtifact::LiveWrappable::Stateless) {
        auto recreate = KJ_ASSERT_NONNULL(recreateFnFor(stored.tag),
            "a stateless slot's tag has no snapshot recreate function",
            static_cast<uint16_t>(stored.tag));
        instance = recreate(js);
      }
      KJ_CASE_ONEOF(clone, kj::Own<Wrappable>) {
        instance = KJ_ASSERT_NONNULL(clone->snapshotClone(), "snapshot clone failed to clone again",
            clone->jsgGetMemoryName());
      }
    }
    instancesBuilder.add(instance.get());
    slots[i].shim = tracer.allocateShimForSnapshotRestore(kj::mv(instance), stored.tag);
  }
  instances = instancesBuilder.finish();
}

void WrappableDeserializeMetadata::relinkReferences(const SnapshotArtifact& artifact) {
  for (auto i: kj::indices(slots)) {
    auto& references = artifact.liveWrappables[i].references;
    if (references.size() == 0 || !slots[i].linked) continue;
    auto restored = kj::heapArrayBuilder<Wrappable*>(references.size());
    for (auto& reference: references) {
      Wrappable* target = nullptr;
      KJ_IF_SOME(slot, reference) {
        if (slots[slot].linked) target = instances[slot];
      }
      restored.add(target);
    }
    instances[i]->snapshotRelink(restored.finish());
  }
}

void WrappableDeserializeMetadata::releaseSlotsWithChecks() {
  // Release the roots whichever way this returns: linked shims are reachable from their
  // wrappers, and unlinked ones must not outlive a failed restore.
  KJ_DEFER(slots = nullptr);

  KJ_REQUIRE(badPayloadsCount == 0,
      "START_FROM_SNAPSHOT: API wrapper payloads in the deserialized context do not match the "
      "artifact",
      badPayloadsCount);

  KJ_REQUIRE(duplicateLinksCount == 0,
      "START_FROM_SNAPSHOT: an artifact slot was referenced by more than one deserialized wrapper",
      duplicateLinksCount);

  for (auto i: kj::indices(slots)) {
    auto& slot = slots[i];
    KJ_REQUIRE(slot.shim.Get() == nullptr || slot.linked,
        "START_FROM_SNAPSHOT: a serialized wrapper slot was never restored", i);
  }
}

v8::StartupData serializeApiWrapperCallback(
    v8::Local<v8::Object> holder, void* cppHeapPointer, void* data) {
  if (cppHeapPointer == nullptr) return {nullptr, 0};

  auto& metadata = *static_cast<WrappablesSerializeMetadata*>(data);
  KJ_IF_SOME(entry, metadata.entries.find(static_cast<const void*>(cppHeapPointer))) {
    if (static_cast<uint16_t>(entry.tag) == kContextGlobalWrappableTag) {
      return {nullptr, 0};
    }

    KJ_IF_SOME(index, entry.index) {
      entry.serialized = true;
      return {reinterpret_cast<const char*>(&index), sizeof(index)};
    }

    // index == kj::none means that there was no snapshotClone() for this obj.
    metadata.unclonable.add(kj::str(entry.typeName));
    return {nullptr, 0};
  }

  ++metadata.unknownWrappersCount;
  return {nullptr, 0};
}

AbortOnGcInDeserializerCallbacks::AbortOnGcInDeserializerCallbacks(
    v8::Isolate* isolate, WrappableDeserializeMetadata& metadata)
    : isolate(isolate),
      metadata(metadata) {
  isolate->AddGCPrologueCallback(&onGcPrologue, &metadata);
}

AbortOnGcInDeserializerCallbacks::~AbortOnGcInDeserializerCallbacks() noexcept(false) {
  isolate->RemoveGCPrologueCallback(&onGcPrologue, &metadata);
}

void AbortOnGcInDeserializerCallbacks::onGcPrologue(
    v8::Isolate* isolate, v8::GCType type, v8::GCCallbackFlags flags, void* data) {
  auto& metadata = *static_cast<WrappableDeserializeMetadata*>(data);
  if (metadata.inCallback) {
    KJ_LOG(FATAL, "START_FROM_SNAPSHOT: GC started inside a context deserializer callback",
        static_cast<int>(type));
    abort();
  }
}

void deserializeApiWrapperCallback(
    v8::Local<v8::Object> holder, v8::StartupData payload, void* data) {
  auto& metadata = *static_cast<WrappableDeserializeMetadata*>(data);
  metadata.inCallback = true;
  KJ_DEFER(metadata.inCallback = false);
  size_t index;
  if (payload.raw_size != sizeof(index)) {
    ++metadata.badPayloadsCount;
    return;
  }
  memcpy(&index, payload.data, sizeof(index));
  if (index >= metadata.slots.size() || metadata.slots[index].shim.Get() == nullptr) {
    ++metadata.badPayloadsCount;
    return;
  }

  auto& slot = metadata.slots[index];
  if (slot.linked) {
    ++metadata.duplicateLinksCount;
    return;
  }

  HeapTracer::getTracer(metadata.isolate).linkRestoredWrapper(*slot.shim, holder);
  slot.linked = true;
}

v8::StartupData serializeInternalFields(v8::Local<v8::Object> holder, int index, void* data) {
  auto& badTagFieldsCount = *static_cast<uint32_t*>(data);
  const void* field = holder->GetAlignedPointerFromInternalField(
      index, static_cast<v8::EmbedderDataTypeTag>(index));
  if (field == nullptr) return {nullptr, 0};

  if (index != Wrappable::WRAPPABLE_TAG_FIELD_INDEX ||
      (field != &Wrappable::WORKERD_WRAPPABLE_TAG &&
          field != &Wrappable::WORKERD_RUST_WRAPPABLE_TAG)) {
    ++badTagFieldsCount;
    return {nullptr, 0};
  }

  // Encode the type = wrappable tag | rust wrappable tag.
  auto* payload = new char[internalFieldTagSizeInBytes];
  memcpy(payload, field, internalFieldTagSizeInBytes);
  return {payload, internalFieldTagSizeInBytes};
}

void deserializeInternalFields(
    v8::Local<v8::Object> holder, int index, v8::StartupData payload, void* data) {
  auto& metadata = *static_cast<WrappableDeserializeMetadata*>(data);
  metadata.inCallback = true;
  KJ_DEFER(metadata.inCallback = false);
  if (index != Wrappable::WRAPPABLE_TAG_FIELD_INDEX ||
      payload.raw_size != internalFieldTagSizeInBytes) {
    ++metadata.badTagFieldsCount;
    return;
  }

  using TagType = kj::RemoveConst<decltype(Wrappable::WORKERD_WRAPPABLE_TAG)>;
  TagType marker = 0;
  memcpy(&marker, payload.data, internalFieldTagSizeInBytes);
  TagType* tagPtr = nullptr;
  switch (marker) {
    case Wrappable::WORKERD_WRAPPABLE_TAG:
      tagPtr = const_cast<TagType*>(&Wrappable::WORKERD_WRAPPABLE_TAG);
      break;
    case Wrappable::WORKERD_RUST_WRAPPABLE_TAG:
      tagPtr = const_cast<TagType*>(&Wrappable::WORKERD_RUST_WRAPPABLE_TAG);
      break;
    default:
      ++metadata.badTagFieldsCount;
      return;
  }

  holder->SetAlignedPointerInInternalField(
      index, tagPtr, static_cast<v8::EmbedderDataTypeTag>(index));
}

}  // namespace workerd::jsg
