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

}  // namespace
}  // namespace workerd
