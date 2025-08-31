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

    auto readBytes = file->readAllBytes(env.js).get<jsg::BufferSource>();
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
}  // namespace
}  // namespace workerd
