#pragma once

#include <kj/filesystem.h>
#include <kj/map.h>

namespace workerd::server {

using kj::uint;

// Implements an index, stored on disk, which maps leaves of a tree to small integers in a stable
// way.
//
// Specifically, this is used to assign numeric IDs to facets of Durable Objects. Each Durable
// Object is potentially composed of a tree of "facets". One facet -- with ID zero -- serves as
// the root facet. All other facets have a parent facet and a name. Names are unique among facets
// with the same parent, but not globally. Each facet is assigned a numeric ID the first time it is
// seen. These IDs are assigned sequentially.
//
// We assume that the total number of facets created for a single Durable Object over its entire
// lifetime will never be very large. Therefore, it is reasonable to store the entire tree index
// in memory, loaded in its entirely at startup. Because of this, entries can simply be stored in
// order by ID (starting with ID 1, since no entry is needed for the root). We also assume that
// it's never necessary to delete an entry -- while a facet itself can be deleted, if a new facet
// is created with the same name, it should use the same ID. Therefore, the index file can be
// append-only, modified only when a never-before-seen facet is created.
//
// The facet index file therefore uses a very simple format. The index is simply a sequence of
// entries, where each entry is composed of:
// * A 2-byte integer specifying the parent ID.
// * A 2-byte integer specifying the length of the name. Note this cannot be zero.
// * The bytes of the name itself (not including any NUL terminator).
//
// Note that the format implicitly limits a Durable Object to have no more than 65536 facets in
// its entire lifetime. An attempt to exceed this limit will throw an exception. If this ever comes
// up in practice, we probably need to rethink the format -- not just the size of the integers, but
// the entire design, as it is not designed for so many facets.
//
// Notice that the index file's design is such that updating the file strictly at append operation.
// This avoids the need for a write-ahead log on updates. It is still possible, in the event of
// a power failure during an update, that the tail of the index will be corrupted. This is OK,
// becaues that tail could not have been relied upon yet. When reading the file, if a nonsensical
// entry is seen (parent ID out-of-range, name overrunning the end of the file, empty name, or
// duplicate entry), the remainder of the file from that point can simply be ignored. In the
// unlikely event that corrupted entries by coincidence appear to be valid, no harm is done -- this
// only has the effect of assigning IDs to names that will never actually be used.
//
// The index file is prefixed with the 8-byte magic number 0xc4cdce5bc5b0ef57. All integers
// (including the magic number) are in host byte order (which is little-endian on all supported
// platforms).
class FacetTreeIndex {
 public:
  // Construct the index, reading the given file to populate the initial index, and then arranging
  // to append new entries to the file as needed.
  FacetTreeIndex(kj::Own<const kj::File> file);

  // Gets the ID for the given facet, assigning it if needed.
  uint getId(uint parent, kj::StringPtr name);

  // For each child of the given parent ID, call the callback.
  template <typename Func>
  void forEachChild(uint parentId, Func&& callback) {
    for (auto& child: entries.range(EntryPtr{parentId, nullptr}, EntryPtr{parentId + 1, nullptr})) {
      KJ_IASSERT(child.parent == parentId);
      uint childId = 1 + (&child - entries.begin());
      callback(childId, child.name);
    }
  }

 private:
  kj::Own<const kj::File> file;

  // Offset at which to write the next entry. Typically points to the end of the file (except when
  // a corrupted tail was detected).
  uint offset = 0;

  struct EntryPtr;

  struct Entry {
    uint parent;
    kj::String name;

    bool operator==(const Entry& other) const = default;
    bool operator<(const Entry& other) const {
      if (parent < other.parent) return true;
      if (parent > other.parent) return false;
      return name < other.name;
    }
    bool operator<(const EntryPtr& other) const {
      if (parent < other.parent) return true;
      if (parent > other.parent) return false;
      return name < other.name;
    }
  };

  struct EntryPtr {
    uint parent;
    kj::StringPtr name;

    bool operator==(const Entry& other) const {
      return parent == other.parent && name == other.name;
    }
  };

  // All entries. Note that there's no need to store the ID of each entry since they are strictly
  // ordered with no erasures. kj::TreeSet is based on kj::Table which maintains the original
  // insertion order (as long as no erasures occur), so the index of any entry can be computed
  // by subtracting `entries.begin()` from its pointer. (Add 1 to the index to get the ID, since
  // the root is ID zero.)
  kj::TreeSet<Entry> entries;

  // Next ID that will be assigned. Off-by-one due to root not being in the set.
  inline uint nextId() {
    return entries.size() + 1;
  }

  static constexpr uint64_t MAGIC_NUMBER = 0xc4cdce5bc5b0ef57;
  static constexpr uint MAX_ID = static_cast<uint16_t>(kj::maxValue);

  struct EntryHeader {
    uint16_t parentId;
    uint16_t nameLength;
  };
};

}  // namespace workerd::server
