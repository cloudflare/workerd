// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "snapshot.h"

#include <workerd/jsg/wrappable.h>

#include <cstring>

namespace workerd::jsg {

constexpr size_t internalFieldTagSizeInBytes = sizeof(Wrappable::WORKERD_WRAPPABLE_TAG);

// The zygote's half of the live-wrappable transfer, under the isolate lock and before
// destroyLiveWrappableInstances(). A snapshotClone() that throws propagates out of
// IsolateBase::createSnapshotBlob() and fails the zygote; the Worker then starts without a
// snapshot.
void WrappablesSerializeMetadata::recordLiveWrappables(HeapTracer& tracer,
    SnapshotArtifact& artifact,
    kj::FunctionParam<bool(v8::CppHeapPointerTag)> isStateless) {
  using LiveWrappable = SnapshotArtifact::LiveWrappable;
  tracer.forEachLiveWrapper([&artifact, &isStateless, this](Wrappable& wrappable,
                                v8::Object::Wrappable& shim, v8::CppHeapPointerTag tag) {
    kj::Maybe<uint32_t> index;
    if (static_cast<uint16_t>(tag) != kContextGlobalWrappableTag) {
      LiveWrappable::Transfer transfer;
      if (isStateless(tag)) {
        transfer = LiveWrappable::Stateless{};
      } else KJ_IF_SOME(clone, wrappable.snapshotClone()) {
        transfer = kj::mv(clone);
      }
      if (transfer != nullptr) {
        index = static_cast<uint32_t>(artifact.liveWrappables.size());
        artifact.liveWrappables.add(LiveWrappable{kj::mv(transfer), tag});
      }
    }
    entries.insert(&shim,
        Entry{.index = index, .tag = tag, .typeName = kj::str(wrappable.jsgGetMemoryName())});
  });
}

void WrappablesSerializeMetadata::verify(SnapshotArtifact& artifact) {
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

  // Drop the transfers of wrappers that were not reachable during snapshot creation: their
  // indices never reached the blob. The slots stay so the serialized indices remain valid.
  for (auto& entry: entries) {
    KJ_IF_SOME(index, entry.value.index) {
      if (!entry.value.serialized) {
        artifact.liveWrappables[index].transfer = SnapshotArtifact::LiveWrappable::Transfer();
      }
    }
  }
}

void WrappableDeserializeMetadata::preallocateCppgcShims(v8::Isolate* v8Isolate,
    HeapTracer& tracer,
    const SnapshotArtifact& artifact,
    kj::Array<kj::Own<Wrappable>> instances) {
  KJ_REQUIRE(instances.size() == artifact.liveWrappables.size());
  isolate = v8Isolate;
  slots = kj::heapArray<Slot>(instances.size());
  for (auto i: kj::indices(instances)) {
    if (instances[i].get() == nullptr) continue;
    slots[i].shim =
        tracer.allocateShimForSnapshotRestore(kj::mv(instances[i]), artifact.liveWrappables[i].tag);
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
  // `cppHeapPointer` is the CppgcShim the wrapper was Wrap()ped with, or null for template
  // instances that were never wrapped.
  if (cppHeapPointer == nullptr) return {nullptr, 0};
  auto& metadata = *static_cast<WrappablesSerializeMetadata*>(data);
  KJ_IF_SOME(entry, metadata.entries.find(static_cast<const void*>(cppHeapPointer))) {
    if (static_cast<uint16_t>(entry.tag) == kContextGlobalWrappableTag) {
      // The hidden JSGlobalObject shares the global's shim. newContext() attaches a fresh global
      // scope on load, so nothing is transferred for it.
      return {nullptr, 0};
    }

    KJ_IF_SOME(index, entry.index) {
      entry.payload = index;
      entry.serialized = true;
      return {reinterpret_cast<const char*>(&entry.payload), sizeof(entry.payload)};
    }

    metadata.unclonable.add(kj::str(entry.typeName));
    return {nullptr, 0};
  }

  ++metadata.unknownWrappersCount;
  return {nullptr, 0};
}

void deserializeApiWrapperCallback(
    v8::Local<v8::Object> holder, v8::StartupData payload, void* data) {
  auto& metadata = *static_cast<WrappableDeserializeMetadata*>(data);
  if (payload.raw_size != sizeof(uint32_t)) {
    ++metadata.badPayloadsCount;
    return;
  }

  uint32_t index;
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
  auto& badTagFieldsCount = *static_cast<uint32_t*>(data);
  if (index != Wrappable::WRAPPABLE_TAG_FIELD_INDEX ||
      payload.raw_size != internalFieldTagSizeInBytes) {
    ++badTagFieldsCount;
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
      ++badTagFieldsCount;
      return;
  }

  holder->SetAlignedPointerInInternalField(
      index, tagPtr, static_cast<v8::EmbedderDataTypeTag>(index));
}

}  // namespace workerd::jsg
