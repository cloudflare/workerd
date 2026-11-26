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

KJ_TEST("fileUrlToPath decodes each pathname segment") {
  auto path = KJ_ASSERT_NONNULL(fileUrlToPath("file:///tmp/a%20b/%C3%A5%25/c"_url));
  KJ_EXPECT(path.size() == 4);
  KJ_EXPECT(path[1] == "a b");
  KJ_EXPECT(path[2] == "\xc3\xa5%");
  KJ_EXPECT(fileUrlToPathString("file:///tmp/a%20b/%C3%A5%25"_url) == "/tmp/a b/\xc3\xa5%");
  KJ_EXPECT(KJ_ASSERT_NONNULL(fileUrlToPath("file:///"_url)).size() == 0);

  // A segment that decodes to something no file name can be is rejected.
  KJ_EXPECT(fileUrlToPath("file:///tmp/a%2Fb"_url) == kj::none);
  KJ_EXPECT(fileUrlToPath("file:///tmp/a%2fb"_url) == kj::none);
  KJ_EXPECT(fileUrlToPath("file:///tmp/a%00b"_url) == kj::none);
}

KJ_TEST("encodeFileName round-trips through URL resolution") {
  auto base = "file:///tmp/"_url;
  for (auto name:
      {"a b"_kj, "p%20q"_kj, "%41"_kj, "h#a?sh"_kj, "b\\s"_kj, "c:d"_kj, "t\tn\nr\r"_kj}) {
    auto url = KJ_ASSERT_NONNULL(base.tryResolve(encodeFileName(name)));
    auto path = KJ_ASSERT_NONNULL(fileUrlToPath(url));
    KJ_EXPECT(path.size() == 2, name, path.size());
    KJ_EXPECT(path[1] == name, name, path[1]);
  }
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
