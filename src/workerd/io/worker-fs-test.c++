#include "worker-fs.h"

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/memory.h>
#include <workerd/jsg/setup.h>
#include <workerd/util/autogate.h>

#include <v8-profiler.h>

#include <kj/debug.h>
#include <kj/test.h>

namespace workerd {
namespace {

jsg::V8System v8System;
class ContextGlobalObject: public jsg::Object, public jsg::ContextGlobal {};

// Holds file system nodes, as an API object does, so that heap snapshots report them.
struct NodeHolder: public jsg::Object {
  kj::Vector<kj::Rc<Directory>> directories;
  kj::Vector<kj::Rc<File>> files;

  JSG_RESOURCE_TYPE(NodeHolder) {}

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    for (auto& dir: directories) {
      tracker.trackField("directory", *dir);
    }
    for (auto& file: files) {
      tracker.trackField("file", *file);
    }
  }
};

struct FsTestContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(FsTestContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(FsTestIsolate, FsTestContext, NodeHolder);

// How many nodes of a heap snapshot of the isolate have each name.
kj::HashMap<kj::String, size_t> countSnapshotNodes(jsg::Lock& js) {
  kj::Vector<char> serialized;
  jsg::HeapSnapshotActivity activity([](auto, auto) { return true; });
  jsg::HeapSnapshotWriter writer([&](kj::Maybe<kj::ArrayPtr<char>> maybeChunk) {
    KJ_IF_SOME(chunk, maybeChunk) {
      serialized.addAll(chunk);
    }
    return true;
  });
  jsg::HeapSnapshotDeleter deleter;
  auto snapshot = kj::Own<const v8::HeapSnapshot>(
      js.v8Isolate->GetHeapProfiler()->TakeHeapSnapshot(&activity, nullptr, true, true), deleter);
  snapshot->Serialize(&writer, v8::HeapSnapshot::kJSON);

  auto root = KJ_ASSERT_NONNULL(
      jsg::JsValue(js.parseJson(serialized.asPtr()).getHandle(js)).tryCast<jsg::JsObject>());
  auto array = [&](jsg::JsObject object, kj::StringPtr name) {
    return KJ_ASSERT_NONNULL(object.get(js, name).tryCast<jsg::JsArray>());
  };
  auto meta = KJ_ASSERT_NONNULL(KJ_ASSERT_NONNULL(root.get(js, "snapshot").tryCast<jsg::JsObject>())
                                    .get(js, "meta")
                                    .tryCast<jsg::JsObject>());
  auto fields = array(meta, "node_fields");
  kj::Maybe<uint32_t> nameField;
  for (uint32_t i = 0; i < fields.size(); i++) {
    if (fields.get(js, i).toString(js) == "name") nameField = i;
  }
  auto nameIndex = KJ_ASSERT_NONNULL(nameField);
  auto nodes = array(root, "nodes");
  auto strings = array(root, "strings");

  kj::HashMap<kj::String, size_t> counts;
  for (uint32_t i = nameIndex; i < nodes.size(); i += fields.size()) {
    auto string =
        KJ_ASSERT_NONNULL(KJ_ASSERT_NONNULL(nodes.get(js, i).tryCast<jsg::JsNumber>()).value(js));
    auto name = strings.get(js, static_cast<uint32_t>(string)).toString(js);
    counts.upsert(kj::mv(name), 1, [](size_t& count, size_t) { count++; });
  }
  return counts;
}

size_t countOf(const kj::HashMap<kj::String, size_t>& counts, kj::StringPtr name) {
  return counts.find(kj::str("workerd / ", name)).orDefault(0);
}

KJ_TEST("Heap snapshots report each file system node once") {
  FsTestIsolate isolate(v8System, kj::heap<jsg::IsolateObserver>());
  isolate.runInLockScope([&](FsTestIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(
        lock, lock.newContext<FsTestContext>().getHandle(lock), [&](jsg::Lock& js) {
      Directory::Builder builder;
      builder.addPath(kj::Path({"sub", "a"}), File::newReadable("abc"_kj.asBytes()));
      builder.addPath(kj::Path({"sub", "b"}), File::newReadable("defg"_kj.asBytes()));
      auto root = builder.finish();

      // Two handles to one directory, and a handle to a file in it.
      auto holder = js.alloc<NodeHolder>();
      for (auto i KJ_UNUSED: kj::zeroTo(2)) {
        auto node = KJ_ASSERT_NONNULL(root->tryOpen(js, kj::Path({"sub"})));
        holder->directories.add(kj::mv(node.get<kj::Rc<Directory>>()));
      }
      auto node = KJ_ASSERT_NONNULL(root->tryOpen(js, kj::Path({"sub", "a"})));
      holder->files.add(kj::mv(node.get<kj::Rc<File>>()));
      auto wrapped KJ_UNUSED = lock.getTypeHandler<jsg::Ref<NodeHolder>>().wrap(js, kj::mv(holder));

      auto counts = countSnapshotNodes(js);
      KJ_EXPECT(countOf(counts, "NodeHolder") == 1);
      KJ_EXPECT(countOf(counts, "Directory") == 1);
      KJ_EXPECT(countOf(counts, "File") == 2);
      // The Rust implementation's handles are nodes of their own, with an edge to the node.
      auto rust = util::Autogate::isEnabled(util::AutogateKey::RUST_VFS);
      KJ_EXPECT(countOf(counts, "RustDirectory") == (rust ? 2 : 0));
      KJ_EXPECT(countOf(counts, "RustFile") == (rust ? 1 : 0));
    });
  });
}

KJ_TEST("A lazy directory's failed load throws from the operation and is retried") {
  FsTestIsolate isolate(v8System, kj::heap<jsg::IsolateObserver>());
  isolate.runInLockScope([&](FsTestIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(
        lock, lock.newContext<FsTestContext>().getHandle(lock), [&](jsg::Lock& js) {
      size_t loads = 0;
      auto lazy = getLazyDirectoryImpl([&loads]() -> kj::Rc<Directory> {
        if (++loads < 6) {
          KJ_FAIL_REQUIRE("loading failed", loads);
        }
        Directory::Builder builder;
        builder.add("f", File::newReadable("x"_kj.asBytes()));
        return builder.finish();
      });

      KJ_EXPECT_THROW_MESSAGE("loading failed", (void)lazy->stat(js, kj::Path({"f"})));
      KJ_EXPECT_THROW_MESSAGE("loading failed", (void)lazy->list(js));
      KJ_EXPECT_THROW_MESSAGE(
          "loading failed", (void)lazy->add(js, "g", File::newReadable("y"_kj.asBytes())));
      KJ_EXPECT_THROW_MESSAGE("loading failed", (void)lazy->remove(js, kj::Path({"f"})));
      KJ_EXPECT_THROW_MESSAGE("loading failed", (void)lazy->getUniqueId(js));
      KJ_EXPECT(loads == 5);
      KJ_EXPECT(lazy->count(js) == 1);
      KJ_EXPECT(lazy->tryOpen(js, kj::Path({"f"})) != kj::none);
      KJ_EXPECT(loads == 6);
    });
  });
}

KJ_TEST("C++ that a link's resolution calls can't check a link against the same guard") {
  FsTestIsolate isolate(v8System, kj::heap<jsg::IsolateObserver>());
  isolate.runInLockScope([&](FsTestIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(
        lock, lock.newContext<FsTestContext>().getHandle(lock), [&](jsg::Lock& js) {
      // The loader runs while the link below resolves through it, and on its first call checks
      // another link against the walk's guard, as a loader that resolved links would.
      kj::Maybe<kj::Rc<SymbolicLink>> other;
      size_t loads = 0;
      auto lazy = getLazyDirectoryImpl([&]() -> kj::Rc<Directory> {
        if (loads++ == 0) {
          KJ_EXPECT(SymbolicLinkRecursionGuardScope::checkSeen(KJ_ASSERT_NONNULL(other).get()) ==
              kj::none);
        }
        Directory::Builder builder;
        builder.add("x", File::newReadable("x"_kj.asBytes()));
        return builder.finish();
      });
      Directory::Builder builder;
      builder.add("lazy", kj::mv(lazy));
      auto root = builder.finish();
      other = kj::rc<SymbolicLink>(root.addRef(), kj::Path({"lazy"}), kj::none);
      auto link = kj::rc<SymbolicLink>(root.addRef(), kj::Path({"lazy", "x"}), kj::none);

      SymbolicLinkRecursionGuardScope scope;
      if (util::Autogate::isEnabled(util::AutogateKey::RUST_VFS)) {
        // Rust holds the guard for the whole resolution, so the check throws, from the
        // resolution once Rust has returned.
        KJ_EXPECT_THROW_MESSAGE("resolved through the same guard", (void)link->resolve(js));
        KJ_EXPECT(loads == 1);
        // The guard is no longer lent: resolving again retries the failed load and succeeds.
        KJ_EXPECT(link->resolve(js) != kj::none);
        KJ_EXPECT(loads == 2);
      } else {
        // C++ keeps the guard in the scope, where the loader's check is one more link seen.
        KJ_EXPECT(link->resolve(js) != kj::none);
        KJ_EXPECT(loads == 1);
      }
    });
  });
}

KJ_TEST("An empty read-only file viewing no bytes") {
  FsTestIsolate isolate(v8System, kj::heap<jsg::IsolateObserver>());
  isolate.runInLockScope([&](FsTestIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(
        lock, lock.newContext<FsTestContext>().getHandle(lock), [&](jsg::Lock& js) {
      auto file = File::newReadable(kj::ArrayPtr<const kj::byte>());
      KJ_EXPECT(file->stat(js).size == 0);
      kj::byte buffer[4]{};
      KJ_EXPECT(file->read(js, 0, buffer) == 0);
      auto bytes =
          KJ_ASSERT_NONNULL(file->readAllBytes(js).tryGet<jsg::JsRef<jsg::JsUint8Array>>());
      KJ_EXPECT(bytes.getHandle(js).size() == 0);
    });
  });
}

// Throws from every notification, after counting it.
struct ThrowingObserver final: public VirtualFileSystem::Observer {
  size_t& calls;
  explicit ThrowingObserver(size_t& calls): calls(calls) {}
  void onOpen(size_t, size_t) const override {
    calls++;
    KJ_FAIL_REQUIRE("onOpen failed");
  }
  void onClose(size_t, size_t) const override {
    calls++;
    KJ_FAIL_REQUIRE("onClose failed");
  }
};

KJ_TEST("An observer's exception throws from the operation, which still takes effect") {
  FsTestIsolate isolate(v8System, kj::heap<jsg::IsolateObserver>());
  isolate.runInLockScope([&](FsTestIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(
        lock, lock.newContext<FsTestContext>().getHandle(lock), [&](jsg::Lock& js) {
      Directory::Builder builder;
      builder.add("f", File::newReadable("x"_kj.asBytes()));
      size_t calls = 0;
      auto vfs = newVirtualFileSystem(
          kj::heap<FsMap>(), builder.finish(), kj::heap<ThrowingObserver>(calls));
      auto url = KJ_ASSERT_NONNULL(jsg::Url::tryParse("file:///f"_kj));

      // The observer's own exception, not one that wraps it.
      auto expectThrows = [](kj::StringPtr description, auto&& func) {
        auto exception = KJ_ASSERT_NONNULL(kj::runCatchingExceptions(func));
        KJ_EXPECT(exception.getDescription() == description, exception);
      };
      expectThrows("onOpen failed", [&] { (void)vfs->openFd(js, url, {}); });
      KJ_EXPECT(calls == 1);
      // The file descriptor was opened before the observer threw.
      auto fd = static_cast<int>(VirtualFileSystem::Stdio::ERR) + 1;
      KJ_EXPECT(vfs->tryGetFd(js, fd) != kj::none);

      expectThrows("onClose failed", [&] { vfs->closeFd(js, fd); });
      KJ_EXPECT(calls == 2);
      KJ_EXPECT(vfs->tryGetFd(js, fd) == kj::none);
    });
  });
}

KJ_TEST("Heap snapshots report a deep tree") {
  FsTestIsolate isolate(v8System, kj::heap<jsg::IsolateObserver>());
  isolate.runInLockScope([&](FsTestIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(
        lock, lock.newContext<FsTestContext>().getHandle(lock), [&](jsg::Lock& js) {
      constexpr size_t kDepth = 300;
      // The root and kDepth - 1 directories in it, the last of which holds a file.
      auto path = kj::Path(KJ_MAP(i KJ_UNUSED, kj::zeroTo(kDepth)) { return kj::str("d"); });
      Directory::Builder builder;
      builder.addPath(path, File::newReadable("x"_kj.asBytes()));

      auto holder = js.alloc<NodeHolder>();
      holder->directories.add(builder.finish());
      auto wrapped KJ_UNUSED = lock.getTypeHandler<jsg::Ref<NodeHolder>>().wrap(js, kj::mv(holder));

      auto counts = countSnapshotNodes(js);
      KJ_EXPECT(countOf(counts, "Directory") == kDepth, countOf(counts, "Directory"));
    });
  });
}

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
