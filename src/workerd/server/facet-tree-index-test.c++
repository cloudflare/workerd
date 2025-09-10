#include "facet-tree-index.h"

#include <kj/io.h>
#include <kj/memory.h>
#include <kj/test.h>
#include <kj/time.h>

namespace workerd::server {
namespace {

using kj::byte;
using kj::uint;

struct ExpectedChildInfo {
  uint id;
  kj::StringPtr name;
};
void expectChildren(
    FacetTreeIndex& index, uint parentId, kj::ArrayPtr<const ExpectedChildInfo> expected) {
  index.forEachChild(parentId, [&](uint id, kj::StringPtr name) {
    if (expected.size() == 0) {
      KJ_FAIL_EXPECT("unexpected child", id, name);
    } else {
      KJ_EXPECT(id == expected.front().id);
      KJ_EXPECT(name == expected.front().name);
      expected = expected.slice(1);
    }
  });
  KJ_EXPECT(expected.size() == 0, "missing child", expected.front().id, expected.front().name);
}

KJ_TEST("FacetTreeIndex basic functionality") {
  auto file = kj::newInMemoryFile(kj::nullClock());

  {
    // Test with new empty file
    FacetTreeIndex index(file->clone());

    // Get IDs for facets
    uint id1 = index.getId(0, "facet1");
    uint id2 = index.getId(0, "facet2");
    uint id3 = index.getId(id1, "child1");
    uint id4 = index.getId(id1, "child2");
    uint id5 = index.getId(id2, "child1");

    // Check that IDs are assigned correctly
    KJ_EXPECT(id1 == 1);
    KJ_EXPECT(id2 == 2);
    KJ_EXPECT(id3 == 3);
    KJ_EXPECT(id4 == 4);
    KJ_EXPECT(id5 == 5);

    // Check that IDs are stable
    KJ_EXPECT(index.getId(0, "facet1") == id1);
    KJ_EXPECT(index.getId(0, "facet2") == id2);
    KJ_EXPECT(index.getId(id1, "child1") == id3);
    KJ_EXPECT(index.getId(id1, "child2") == id4);
    KJ_EXPECT(index.getId(id2, "child1") == id5);

    // Test forEachChild().
    expectChildren(index, 0, {{1, "facet1"}, {2, "facet2"}});
    expectChildren(index, 1, {{3, "child1"}, {4, "child2"}});
    expectChildren(index, 2, {{5, "child1"}});
    expectChildren(index, 3, {});
    expectChildren(index, 4, {});
    expectChildren(index, 5, {});
  }

  {
    // Test with existing file (persistence)
    FacetTreeIndex index(file->clone());

    // Check that IDs are the same as before
    KJ_EXPECT(index.getId(0, "facet1") == 1);
    KJ_EXPECT(index.getId(0, "facet2") == 2);
    KJ_EXPECT(index.getId(1, "child1") == 3);
    KJ_EXPECT(index.getId(1, "child2") == 4);
    KJ_EXPECT(index.getId(2, "child1") == 5);

    // Add some new facets
    uint id6 = index.getId(3, "grandchild1");
    uint id7 = index.getId(3, "grandchild2");

    KJ_EXPECT(id6 == 6);
    KJ_EXPECT(id7 == 7);

    expectChildren(index, 0, {{1, "facet1"}, {2, "facet2"}});
    expectChildren(index, 1, {{3, "child1"}, {4, "child2"}});
    expectChildren(index, 2, {{5, "child1"}});
    expectChildren(index, 3, {{6, "grandchild1"}, {7, "grandchild2"}});
    expectChildren(index, 4, {});
    expectChildren(index, 5, {});
    expectChildren(index, 6, {});
    expectChildren(index, 7, {});
  }

  {
    // Test again with existing file
    FacetTreeIndex index(file->clone());

    // Check all IDs were preserved
    KJ_EXPECT(index.getId(0, "facet1") == 1);
    KJ_EXPECT(index.getId(0, "facet2") == 2);
    KJ_EXPECT(index.getId(1, "child1") == 3);
    KJ_EXPECT(index.getId(1, "child2") == 4);
    KJ_EXPECT(index.getId(2, "child1") == 5);
    KJ_EXPECT(index.getId(3, "grandchild1") == 6);
    KJ_EXPECT(index.getId(3, "grandchild2") == 7);

    expectChildren(index, 0, {{1, "facet1"}, {2, "facet2"}});
    expectChildren(index, 1, {{3, "child1"}, {4, "child2"}});
    expectChildren(index, 2, {{5, "child1"}});
    expectChildren(index, 3, {{6, "grandchild1"}, {7, "grandchild2"}});
    expectChildren(index, 4, {});
    expectChildren(index, 5, {});
    expectChildren(index, 6, {});
    expectChildren(index, 7, {});
  }
}

KJ_TEST("FacetTreeIndex error handling") {
  auto file = kj::newInMemoryFile(kj::nullClock());
  FacetTreeIndex index(file->clone());

  // Add some initial facets
  index.getId(0, "facet1");
  index.getId(0, "facet2");

  // Test error cases

  // Empty name
  KJ_EXPECT_THROW_MESSAGE("Facet name cannot be empty", index.getId(0, ""));

  // Invalid parent
  KJ_EXPECT_THROW_MESSAGE("Invalid parent ID", index.getId(999, "child"));

  // Same name but different parents should get different IDs
  uint id1 = index.getId(1, "sameName");
  uint id2 = index.getId(2, "sameName");
  KJ_EXPECT(id1 != id2);

  // Test name uniqueness per parent
  uint id3 = index.getId(1, "sameName");
  KJ_EXPECT(id3 == id1);
}

KJ_TEST("FacetTreeIndex corruption handling") {
  auto file = kj::newInMemoryFile(kj::nullClock());

  // Create a file with corrupted data
  {
    // Write valid header and some valid entries
    constexpr uint64_t MAGIC_NUMBER = 0xc4cdce5bc5b0ef57;
    file->write(0, kj::ArrayPtr<const byte>((const byte*)&MAGIC_NUMBER, sizeof(MAGIC_NUMBER)));

    // Write valid entry: parent=0, name="valid"
    uint16_t parent = 0;
    uint16_t nameLen = 5;
    byte entry[4 + 5] = {0};
    memcpy(entry, &parent, 2);
    memcpy(entry + 2, &nameLen, 2);
    memcpy(entry + 4, "valid", 5);
    file->write(sizeof(MAGIC_NUMBER), kj::ArrayPtr<const byte>(entry, sizeof(entry)));

    // Write corrupted entry: parent=999 (invalid), name="corrupt"
    uint16_t badParent = 999;
    uint16_t badNameLen = 7;
    byte badEntry[4 + 7] = {0};
    memcpy(badEntry, &badParent, 2);
    memcpy(badEntry + 2, &badNameLen, 2);
    memcpy(badEntry + 4, "corrupt", 7);
    file->write(
        sizeof(MAGIC_NUMBER) + sizeof(entry), kj::ArrayPtr<const byte>(badEntry, sizeof(badEntry)));

    // Write valid entry after corruption that should be ignored
    uint16_t ignoredParent = 0;
    uint16_t ignoredNameLen = 7;
    byte ignoredEntry[4 + 7] = {0};
    memcpy(ignoredEntry, &ignoredParent, 2);
    memcpy(ignoredEntry + 2, &ignoredNameLen, 2);
    memcpy(ignoredEntry + 4, "ignored", 7);
    file->write(sizeof(MAGIC_NUMBER) + sizeof(entry) + sizeof(badEntry),
        kj::ArrayPtr<const byte>(ignoredEntry, sizeof(ignoredEntry)));
  }

  // Open corrupted file
  {
    FacetTreeIndex index(file->clone());

    // Check that only valid entries were read
    KJ_EXPECT(index.getId(0, "valid") == 1);

    // The corrupted entry and everything after it should have been ignored
    // So this should create a new entry
    uint id = index.getId(0, "corrupt");
    KJ_EXPECT(id == 2);

    // Similarly, "ignored" should be new
    uint id2 = index.getId(0, "ignored");
    KJ_EXPECT(id2 == 3);
  }

  // Open yet again, make sure that the newly-added entries were written successfully.
  {
    FacetTreeIndex index(file->clone());
    KJ_EXPECT(index.getId(0, "valid") == 1);
    KJ_EXPECT(index.getId(0, "corrupt") == 2);
    KJ_EXPECT(index.getId(0, "ignored") == 3);
  }
}

KJ_TEST("FacetTreeIndex tree structure") {
  auto file = kj::newInMemoryFile(kj::nullClock());

  FacetTreeIndex index(file->clone());

  // Build a tree with multiple levels
  uint id1 = index.getId(0, "root1");
  uint id2 = index.getId(0, "root2");

  uint id3 = index.getId(id1, "level1_1");
  uint id4 = index.getId(id1, "level1_2");
  uint id5 = index.getId(id2, "level1_3");

  uint id6 = index.getId(id3, "level2_1");
  uint id7 = index.getId(id3, "level2_2");
  uint id8 = index.getId(id4, "level2_3");

  uint id9 = index.getId(id6, "level3_1");

  // Verify IDs
  KJ_EXPECT(id1 == 1);
  KJ_EXPECT(id2 == 2);
  KJ_EXPECT(id3 == 3);
  KJ_EXPECT(id4 == 4);
  KJ_EXPECT(id5 == 5);
  KJ_EXPECT(id6 == 6);
  KJ_EXPECT(id7 == 7);
  KJ_EXPECT(id8 == 8);
  KJ_EXPECT(id9 == 9);

  // Verify stable lookup
  KJ_EXPECT(index.getId(id1, "level1_1") == id3);
  KJ_EXPECT(index.getId(id3, "level2_1") == id6);
  KJ_EXPECT(index.getId(id6, "level3_1") == id9);
}

KJ_TEST("FacetTreeIndex handles truncated files correctly") {
  auto file = kj::newInMemoryFile(kj::nullClock());

  // Step 1: Create a file with a few entries
  {
    FacetTreeIndex index(file->clone());
    uint id1 = index.getId(0, "entry1");
    uint id2 = index.getId(0, "entry2");
    uint id3 = index.getId(0, "entry3");

    KJ_EXPECT(id1 == 1);
    KJ_EXPECT(id2 == 2);
    KJ_EXPECT(id3 == 3);
  }

  // Step 2: Corrupt the last entry by overwriting its nameLength field with an invalid large value
  auto fileSize = file->stat().size;
  uint offset =
      fileSize - 8;  // Go to the nameLength field of the last entry (2 bytes before "entry3")

  // Write an impossibly large nameLength value
  uint16_t hugeNameLength = 65000;  // Much larger than any valid name in our test file
  file->write(
      offset, kj::ArrayPtr<const byte>((const byte*)&hugeNameLength, sizeof(hugeNameLength)));

  // Step 3: Re-read the index and add a new entry
  {
    FacetTreeIndex index(file->clone());

    // First two entries should still be valid
    KJ_EXPECT(index.getId(0, "entry1") == 1);
    KJ_EXPECT(index.getId(0, "entry2") == 2);

    // The corrupted entry (entry3) should not be found, so this new entry
    // should get the ID 3 (reusing the ID that was intended for entry3)
    uint id = index.getId(0, "replacement");
    KJ_EXPECT(id == 3);
  }

  // Step 4: Re-read the file again and add yet another new entry
  {
    FacetTreeIndex index(file->clone());

    // Immediately get a new entry, without checking existing ones first
    // This should get ID 4, not reuse ID 3 again
    uint id = index.getId(0, "another");

    // Now check that all previous entries are remembered
    KJ_EXPECT(id == 4);
    KJ_EXPECT(index.getId(0, "entry1") == 1);
    KJ_EXPECT(index.getId(0, "entry2") == 2);
    KJ_EXPECT(index.getId(0, "replacement") == 3);
  }
}

}  // namespace
}  // namespace workerd::server
