#include "worker-fs.h"

#include <kj/debug.h>
#include <kj/test.h>

namespace workerd {
namespace {

kj::Own<FsMap> createTestFsMap() {
  auto map = kj::heap<FsMap>();
  map->setBundleRoot("/mything/bundle");
  map->setTempRoot("/mything/temp");
  return kj::mv(map);
}

KJ_TEST("FsMap") {
  auto fsMap = createTestFsMap();

  // Check that the paths are correct.
  KJ_EXPECT(fsMap->getBundlePath().toString(false) == "mything/bundle");
  KJ_EXPECT(fsMap->getTempPath().toString(false) == "mything/temp");
  KJ_EXPECT(fsMap->getBundleRoot().equal("file:///mything/bundle"_url));
  KJ_EXPECT(fsMap->getTempRoot().equal("file:///mything/temp"_url));
}

KJ_TEST("TmpDirStoreScope") {
  // We can create multiple temp storages on the heap...
  auto tmpStoreOnHeap = TmpDirStoreScope::create();
  auto tmpStoreOnHeap2 = TmpDirStoreScope::create();

  KJ_EXPECT(!TmpDirStoreScope::hasCurrent());

  {
    // But we can only have one on the stack at a time per thread.
    TmpDirStoreScope tmpDirStoreScope;
    KJ_EXPECT(TmpDirStoreScope::hasCurrent());
    KJ_ASSERT(&TmpDirStoreScope::current() == &tmpDirStoreScope);
    KJ_ASSERT(&TmpDirStoreScope::current() != tmpStoreOnHeap.get());
    KJ_ASSERT(&TmpDirStoreScope::current() != tmpStoreOnHeap2.get());
  }
  KJ_EXPECT(!TmpDirStoreScope::hasCurrent());
}

KJ_TEST("Directory::Builder::addPath handles deep paths without stack overflow") {
  // Regression test for AUTOVULN-CLOUDFLARE-WORKERD-104: Directory::Builder::addPath
  // used unbounded recursion (one stack frame per path segment). An attacker-controlled
  // module name with ~100,000 segments would exhaust the native stack and SIGSEGV the
  // process. The fix converts addPath to iterative descent.

  // Build a path with 2000 segments. The pre-patch recursive addPath used one
  // native stack frame per segment (with findOrCreate + KJ_SWITCH_ONEOF overhead
  // per frame), so a few thousand segments would overflow the stack. The
  // post-patch iterative version handles this in O(1) stack space.
  kj::Vector<kj::String> segments(2001);
  for (size_t i = 0; i < 2000; i++) {
    segments.add(kj::str("d", i));
  }
  segments.add(kj::str("leaf.txt"));

  kj::Path deepPath(segments.releaseAsArray());

  Directory::Builder builder;
  auto fileData = "hello"_kjb;
  builder.addPath(deepPath, File::newReadable(fileData));

  // Finalize the directory. The fact that we reach this point without SIGSEGV
  // is the primary assertion — pre-patch, the recursive addPath would have
  // exhausted the native stack and crashed the process.
  auto dir = builder.finish();

  // Also verify a shallow path still works correctly after the refactor.
  Directory::Builder builder2;
  auto fileData2 = "world"_kjb;
  kj::Path shallowPath({"a", "b", "c.txt"});
  builder2.addPath(shallowPath, File::newReadable(fileData2));
  auto dir2 = builder2.finish();
  // dir2 should have one top-level entry "a".
  size_t topLevelCount = 0;
  for (auto& _ KJ_UNUSED: *dir2.get()) {
    topLevelCount++;
  }
  KJ_EXPECT(topLevelCount == 1);
}

}  // namespace
}  // namespace workerd
