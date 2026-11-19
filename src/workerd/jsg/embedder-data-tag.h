// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// The embedder data slots workerd uses in v8::Context and the v8::EmbedderDataTypeTags its
// aligned pointers are stored with. Depends on nothing but V8, so that code which stores aligned
// pointers in V8 objects without depending on the jsg library proper can claim a tag too.

#include <v8-object.h>

namespace workerd::jsg {

// The ContextPointerSlot enum defines the embedder slots we use in v8::Context for
// storing pointers to various important objects.
enum class ContextPointerSlot : int {
  // Pointer slot 0 is special and should never be used by us.
  RESERVED = 0,
  MODULE_REGISTRY = 1,
  EXTENDED_CONTEXT_WRAPPER = 2,
  VIRTUAL_FILE_SYSTEM = 3,
  BOOTSTRAP_STATE = 4,
  // Keep the MAX_POINTER_SLOT as the last entry and always set to
  // to the highest value of the other entries. We use this to
  // ensure that the highest used index is always initialized in
  // every context we create without having to update the specific
  // callsites whenever we add a new slot. We can just make the
  // change here.
  MAX_POINTER_SLOT = BOOTSTRAP_STATE,
};

// Every v8::EmbedderDataTypeTag workerd uses. V8 type-checks an aligned pointer stored in
// embedder data or an internal field against the tag it was stored with, so a pointer stored
// under one tag cannot be read back under another. The tags separate the wrappable marker from
// the context pointer slots and the slots from each other; a slot's reader must still use the
// type that was stored in it (MODULE_REGISTRY, for one, holds either module registry). The range
// is 0..V8_EMBEDDER_DATA_TAG_COUNT-1; a new tag goes here, after the last one, never as a literal
// at the use site.
enum class EmbedderDataTag : v8::EmbedderDataTypeTag {
  // The marker in internal field Wrappable::WRAPPABLE_TAG_FIELD_INDEX that identifies an object
  // as a jsg wrapper (see Wrappable::isWorkerdApiObject()).
  WRAPPABLE_MARKER = 0,
  // The pointers in a context's ContextPointerSlots, each tagged with its slot index.
  CONTEXT_POINTER_SLOT_FIRST = 1,
  CONTEXT_POINTER_SLOT_LAST = static_cast<int>(ContextPointerSlot::MAX_POINTER_SLOT),
  // Keep this the highest tag.
  MAX_TAG = CONTEXT_POINTER_SLOT_LAST,
};

static_assert(static_cast<int>(EmbedderDataTag::MAX_TAG) < V8_EMBEDDER_DATA_TAG_COUNT,
    "out of embedder data type tags; raise V8_EMBEDDER_DATA_TAG_COUNT in the V8 build");

constexpr v8::EmbedderDataTypeTag embedderDataTag(EmbedderDataTag tag) {
  return static_cast<v8::EmbedderDataTypeTag>(tag);
}

constexpr v8::EmbedderDataTypeTag contextPointerSlotTag(ContextPointerSlot slot) {
  return static_cast<v8::EmbedderDataTypeTag>(slot);
}
static_assert(contextPointerSlotTag(ContextPointerSlot::MODULE_REGISTRY) ==
    embedderDataTag(EmbedderDataTag::CONTEXT_POINTER_SLOT_FIRST));
static_assert(contextPointerSlotTag(ContextPointerSlot::MAX_POINTER_SLOT) ==
    embedderDataTag(EmbedderDataTag::CONTEXT_POINTER_SLOT_LAST));

}  // namespace workerd::jsg
