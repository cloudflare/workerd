// Copyright (c) 2024-2029 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/bundle-fs.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/test.h>

namespace workerd {
namespace {
workerd::WorkerSource readConfig() {

  kj::Vector<WorkerSource::Module> modules(8);

  modules.add(WorkerSource::Module{.name = "a/esModule"_kj,
    .content = WorkerSource::EsModule{.body = "this is an esm module"_kj}});

  modules.add(WorkerSource::Module{.name = "a/commonJsModule"_kj,
    .content = WorkerSource::CommonJsModule{.body = "this is a commonjs module"_kj}});

  modules.add(WorkerSource::Module{
    .name = "b/text"_kj, .content = WorkerSource::TextModule{.body = "this is a text module"_kj}});

  modules.add(WorkerSource::Module{.name = "b/data"_kj,
    .content = WorkerSource::DataModule{.body = "this is a data module"_kj.asArray().asBytes()}});

  modules.add(WorkerSource::Module{.name = "c/wasm"_kj,
    .content = WorkerSource::WasmModule{.body = "this is a wasm module"_kj.asArray().asBytes()}});

  modules.add(WorkerSource::Module{
    .name = "c/json"_kj, .content = WorkerSource::JsonModule{.body = "this is a json module"_kj}});

  modules.add(WorkerSource::Module{.name = "a/pythonModule"_kj,
    .content = WorkerSource::PythonModule{.body = "this is a python module"_kj}});

  return workerd::WorkerSource(workerd::WorkerSource::ModulesSource{
    .mainModule = "worker"_kj, .modules = modules.releaseAsArray()});
}

KJ_TEST("The BundleDirectoryDelegate works") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto config = readConfig();
    auto dir = getBundleDirectory(config);

    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"a"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"a", "esModule"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"a", "commonJsModule"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"b"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"b", "text"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"b", "data"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"c"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"c", "wasm"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"c", "json"})));
    KJ_EXPECT(dir->tryOpen(env.js, kj::Path({"a", "foo"})) == kj::none);
    KJ_EXPECT(dir->tryOpen(env.js, kj::Path({"zzz", "yyy"})) == kj::none);

    // Iterating over the directory should work.
    size_t counter = 0;
    for (auto& _ KJ_UNUSED: *dir.get()) {
      counter++;
    }
    KJ_EXPECT(counter, 3);

    KJ_EXPECT(dir->count(env.js) == 3);

    auto maybeEsModule = dir->tryOpen(env.js, kj::Path({"a", "esModule"}));
    auto& esmodule = KJ_ASSERT_NONNULL(maybeEsModule).get<kj::Rc<File>>();
    auto stat = esmodule->stat(env.js);
    KJ_EXPECT(stat.type == FsType::FILE);
    KJ_EXPECT(stat.size == 21);

    auto maybeFile = dir->tryOpen(env.js, kj::Path({"a", "commonJsModule"}));
    auto& file = KJ_ASSERT_NONNULL(maybeFile).get<kj::Rc<File>>();

    auto readText = file->readAllText(env.js).get<jsg::JsString>();
    KJ_EXPECT(readText == env.js.str("this is a commonjs module"_kj));

    auto readBytes = file->readAllBytes(env.js).get<jsg::JsUint8Array>();
    KJ_EXPECT(readBytes.asArrayPtr() == "this is a commonjs module"_kjb);

    // Reading five bytes from offset 20 should return "odule".
    kj::byte buffer[5]{};
    size_t r = file->read(env.js, 20, buffer);
    KJ_EXPECT(r == 5);
    KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, r) == "odule"_kjb);

    // Attempt to read beyond EOF return nothing.
    KJ_EXPECT(file->read(env.js, 100, buffer) == 0);

    // Attempts to modify anything should fail.
    auto error = dir->remove(env.js, kj::Path({"a", "esModule"})).get<FsError>();
    KJ_EXPECT(error == FsError::READ_ONLY);

    // Attempting to create a file should fail.
    auto maybeErr = dir->tryOpen(env.js, kj::Path({"a", "something", "else"}),
        Directory::OpenOptions{
          .createAs = FsType::FILE,
        });
    KJ_EXPECT(KJ_ASSERT_NONNULL(maybeErr).get<FsError>() == FsError::READ_ONLY);
  });
}

KJ_TEST("Guarding against circular symlinks works") {
  // This isn't the best location for this particular test since it is
  // not specific to bundle-fs. However, the test needs the TestFixture
  // and it's a bit of a pain to move it to the other test file so for
  // now it will live here.
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    // We don't need to set up a TmpDirStorageScope since we are running
    // within a test fixture that sets up an IoContext...
    auto vfs = newVirtualFileSystem(kj::heap<FsMap>(), getTmpDirectoryImpl());

    // Set up circular symlinks
    auto maybeTemp = KJ_ASSERT_NONNULL(vfs->resolve(env.js, "file:///"_url));
    auto& tempDir = maybeTemp.get<kj::Rc<Directory>>();

    KJ_EXPECT(tempDir->add(env.js, "a", vfs->newSymbolicLink(env.js, "file:///b"_url)) == kj::none);
    KJ_EXPECT(tempDir->add(env.js, "b", vfs->newSymbolicLink(env.js, "file:///c"_url)) == kj::none);
    KJ_EXPECT(tempDir->add(env.js, "c", vfs->newSymbolicLink(env.js, "file:///a"_url)) == kj::none);
    KJ_EXPECT(tempDir->add(env.js, "d", vfs->newSymbolicLink(env.js, "file:///e"_url)) == kj::none);

    // This symlink goes no where. A kj::none should be returned.
    KJ_EXPECT(vfs->resolve(env.js, "file:///d"_url) == kj::none);

    auto resolved = KJ_ASSERT_NONNULL(vfs->resolve(env.js, "file:///a"_url));
    KJ_EXPECT(resolved.get<workerd::FsError>() == workerd::FsError::SYMLINK_DEPTH_EXCEEDED);

    auto resolvedStat = KJ_ASSERT_NONNULL(vfs->resolveStat(env.js, "file:///a"_url));
    KJ_EXPECT(resolvedStat.get<workerd::FsError>() == workerd::FsError::SYMLINK_DEPTH_EXCEEDED);

    resolved = KJ_ASSERT_NONNULL(vfs->resolve(env.js, "file:///b"_url));
    KJ_EXPECT(resolved.get<workerd::FsError>() == workerd::FsError::SYMLINK_DEPTH_EXCEEDED);

    resolved = KJ_ASSERT_NONNULL(vfs->resolve(env.js, "file:///c"_url));
    KJ_EXPECT(resolved.get<workerd::FsError>() == workerd::FsError::SYMLINK_DEPTH_EXCEEDED);

    // And while we're at it, let's check that a symlink can be removed
    KJ_EXPECT(tempDir->remove(env.js, kj::Path({"a"})).get<bool>());
    // Removing the symlink means that the cycle is broken and a resolve for
    // c or b should return kj::none.
    KJ_EXPECT(vfs->resolve(env.js, "file:///a"_url) == kj::none);
    KJ_EXPECT(vfs->resolve(env.js, "file:///b"_url) == kj::none);
    KJ_EXPECT(vfs->resolve(env.js, "file:///c"_url) == kj::none);
  });
}

KJ_TEST("Guarding against deep non-circular symlink chains works") {
  // Regression test: a deep chain of distinct symlinks (no cycle) must not
  // cause a stack overflow. The recursion guard should reject the chain once
  // the depth limit is exceeded.
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto vfs = newVirtualFileSystem(kj::heap<FsMap>(), getTmpDirectoryImpl());

    auto maybeTemp = KJ_ASSERT_NONNULL(vfs->resolve(env.js, "file:///"_url));
    auto& tempDir = maybeTemp.get<kj::Rc<Directory>>();

    // Create a target file at the end of the chain.
    auto targetResult = KJ_ASSERT_NONNULL(tempDir->tryOpen(
        env.js, kj::Path({"target"}), Directory::OpenOptions{.createAs = FsType::FILE}));
    auto& targetFile = targetResult.get<kj::Rc<File>>();
    KJ_EXPECT(targetFile->write(env.js, 0, "hello"_kjb).get<uint32_t>() == 5);

    // Build a chain of 300 distinct symlinks: link_0 -> link_1 -> ... -> link_299 -> target.
    // This exceeds the depth limit (256) without forming a cycle.
    constexpr int chainLen = 300;
    for (int i = chainLen - 1; i >= 0; i--) {
      kj::String target =
          i == chainLen - 1 ? kj::str("file:///target") : kj::str("file:///link_", i + 1);
      auto targetUrl = KJ_ASSERT_NONNULL(jsg::Url::tryParse(target.asPtr()));
      KJ_EXPECT(tempDir->add(env.js, kj::str("link_", i),
                    vfs->newSymbolicLink(env.js, targetUrl)) == kj::none);
    }

    // Resolving a link near the end of the chain should succeed (under the limit).
    KJ_ASSERT_NONNULL(vfs->resolve(env.js, "file:///link_299"_url));

    // Resolving from the start of the chain must fail with SYMLINK_DEPTH_EXCEEDED,
    // not crash with a stack overflow.
    auto resolved = KJ_ASSERT_NONNULL(vfs->resolve(env.js, "file:///link_0"_url));
    KJ_EXPECT(resolved.get<workerd::FsError>() == workerd::FsError::SYMLINK_DEPTH_EXCEEDED);

    // stat should also fail gracefully.
    auto resolvedStat = KJ_ASSERT_NONNULL(vfs->resolveStat(env.js, "file:///link_0"_url));
    KJ_EXPECT(resolvedStat.get<workerd::FsError>() == workerd::FsError::SYMLINK_DEPTH_EXCEEDED);
  });
}

KJ_TEST("Module names exceeding max bundle path depth are skipped") {
  // Regression test for AUTOVULN-CLOUDFLARE-WORKERD-104: an attacker-controlled
  // module name with deeply nested path segments (e.g. "a/".repeat(100000) + "x.txt")
  // could cause stack exhaustion via recursive directory building.
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Vector<WorkerSource::Module> modules(3);

    // A normal module that should be included.
    modules.add(WorkerSource::Module{
      .name = "ok/module.js"_kj,
      .content = WorkerSource::EsModule{.body = "export default 1;"_kj},
    });

    kj::Vector<char> atLimit;
    for (size_t i = 0; i < 255; i++) {
      atLimit.addAll(kj::StringPtr("d/"));
    }
    atLimit.addAll(kj::StringPtr("leaf.txt"));
    atLimit.add('\0');
    kj::StringPtr atLimitName(atLimit.begin(), atLimit.size() - 1);
    modules.add(WorkerSource::Module{
      .name = atLimitName,
      .content = WorkerSource::EsModule{.body = "export default 2;"_kj},
    });

    kj::Vector<char> overLimit;
    for (size_t i = 0; i < 2000; i++) {
      overLimit.addAll(kj::StringPtr("x/"));
    }
    overLimit.addAll(kj::StringPtr("leaf.txt"));
    overLimit.add('\0');
    kj::StringPtr overLimitName(overLimit.begin(), overLimit.size() - 1);
    modules.add(WorkerSource::Module{
      .name = overLimitName,
      .content = WorkerSource::EsModule{.body = "export default 3;"_kj},
    });

    auto config = WorkerSource(WorkerSource::ModulesSource{
      .mainModule = "ok/module.js"_kj,
      .modules = modules.releaseAsArray(),
    });
    auto dir = getBundleDirectory(config);

    // The normal module should be accessible.
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"ok", "module.js"})));

    // The 256-segment module should be accessible — build the lookup path.
    kj::Vector<kj::String> atLimitSegments;
    for (size_t i = 0; i < 255; i++) {
      atLimitSegments.add(kj::str("d"));
    }
    atLimitSegments.add(kj::str("leaf.txt"));
    kj::Path atLimitPath(atLimitSegments.releaseAsArray());
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, atLimitPath));

    // The too deep module should have been skipped entirely. Verify that
    // it is not reachable. We just need to check that the leaf doesn't exist —
    // if the module was skipped, looking up any part of the deep path will
    // return none.
    kj::Vector<kj::String> overLimitSegments;
    for (size_t i = 0; i < 2000; i++) {
      overLimitSegments.add(kj::str("x"));
    }
    overLimitSegments.add(kj::str("leaf.txt"));
    kj::Path overLimitPath(overLimitSegments.releaseAsArray());
    KJ_EXPECT(dir->tryOpen(env.js, overLimitPath) == kj::none);
  });
}

}  // namespace
}  // namespace workerd
