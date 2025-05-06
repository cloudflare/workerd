// Copyright (c) 2024-2029 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/server/bundle-fs.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/test.h>

namespace workerd {
namespace {
kj::Own<server::config::Worker::Reader> readConfig() {
  capnp::MallocMessageBuilder builder;
  auto root = builder.initRoot<server::config::Worker>();
  auto modules = root.initModules(8);

  modules[0].setName("a/esModule");
  modules[0].setEsModule("this is an esm module");

  modules[1].setName("a/commonJsModule");
  modules[1].setCommonJsModule("this is a commonjs module");

  modules[2].setName("b/text");
  modules[2].setText("this is a text module");

  modules[3].setName("b/data");
  modules[3].setWasm("this is a wasm module"_kj.asArray().asBytes());

  modules[4].setName("c/wasm");
  modules[4].setWasm("this is a wasm module"_kj.asArray().asBytes());

  modules[5].setName("c/json");
  modules[5].setJson("this is a json module");

  modules[6].setName("a/pythonModule");
  modules[6].setPythonModule("this is a python module");

  modules[7].setName("emptyModule");
  modules[7].setText("nothing");

  return capnp::clone(root.asReader());
}

KJ_TEST("The BundleDirectoryDelegate works") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto config = readConfig();
    auto dir = server::getBundleDirectory(*config);

    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"a"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"a", "esModule"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"a", "commonJsModule"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"b"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"b", "text"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"b", "data"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"c"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"c", "wasm"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"c", "json"})));
    KJ_REQUIRE_NONNULL(dir->tryOpen(env.js, kj::Path({"emptyModule"})));
    KJ_EXPECT(dir->tryOpen(env.js, kj::Path({"emptyModule", "text"})) == kj::none);
    KJ_EXPECT(dir->tryOpen(env.js, kj::Path({"a", "foo"})) == kj::none);
    KJ_EXPECT(dir->tryOpen(env.js, kj::Path({"zzz", "yyy"})) == kj::none);

    // Iterating over the directory should work.
    size_t counter = 0;
    for (auto& _ KJ_UNUSED: *dir.get()) {
      counter++;
    }
    KJ_EXPECT(counter, 4);

    KJ_EXPECT(dir->count(env.js) == 4);

    auto maybeEsModule = dir->tryOpen(env.js, kj::Path({"a", "esModule"}));
    auto& esmodule = KJ_ASSERT_NONNULL(maybeEsModule).get<kj::Rc<File>>();
    auto stat = esmodule->stat(env.js);
    KJ_EXPECT(stat.type == FsType::FILE);
    KJ_EXPECT(stat.size == 21);

    auto maybeFile = dir->tryOpen(env.js, kj::Path({"a", "commonJsModule"}));
    auto& file = KJ_ASSERT_NONNULL(maybeFile).get<kj::Rc<File>>();
    KJ_EXPECT(file->readAllText(env.js) == env.js.str("this is a commonjs module"_kj));
    KJ_EXPECT(file->readAllBytes(env.js).asArrayPtr() == "this is a commonjs module"_kjb);

    // Reading five bytes from offset 20 should return "odule".
    kj::byte buffer[5]{};
    size_t r = file->read(env.js, 20, buffer);
    KJ_EXPECT(r == 5);
    KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, r) == "odule"_kjb);

    // Attempt to read beyond EOF return nothing.
    KJ_EXPECT(file->read(env.js, 100, buffer) == 0);

    // Attempts to modify anything should fail.
    try {
      dir->remove(env.js, kj::Path({"a", "esModule"}));
      KJ_FAIL_ASSERT("should have failed");
    } catch (...) {
      auto ex = kj::getCaughtExceptionAsKj();
      KJ_EXPECT(ex.getDescription() ==
          "jsg.Error: Cannot remove a file or directory from a read-only directory");
    }

    // Attempting to create a file should fail.
    try {
      dir->tryOpen(env.js, kj::Path({"a", "something", "else"}),
          Directory::OpenOptions{
            .createAs = FsType::FILE,
          });
      KJ_FAIL_ASSERT("should have failed");
    } catch (...) {
      auto ex = kj::getCaughtExceptionAsKj();
      KJ_EXPECT(ex.getDescription().endsWith(
          "jsg.Error: Cannot create a new file or directory in a read-only directory"));
    }
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

    tempDir->add(env.js, "a", vfs->newSymbolicLink(env.js, "file:///b"_url));
    tempDir->add(env.js, "b", vfs->newSymbolicLink(env.js, "file:///c"_url));
    tempDir->add(env.js, "c", vfs->newSymbolicLink(env.js, "file:///a"_url));
    tempDir->add(env.js, "d", vfs->newSymbolicLink(env.js, "file:///e"_url));

    // This symlink goes no where. A kj::none should be returned.
    KJ_EXPECT(vfs->resolve(env.js, "file:///d"_url) == kj::none);

    try {
      // This symlink is circular. It should throw an exception.
      vfs->resolve(env.js, "file:///a"_url);
      KJ_FAIL_ASSERT("should have failed");
    } catch (...) {
      auto ex = kj::getCaughtExceptionAsKj();
      KJ_EXPECT(ex.getDescription().endsWith("jsg.Error: Recursive symbolic link detected"));
    }

    try {
      // This symlink is circular. It should throw an exception.
      vfs->resolveStat(env.js, "file:///a"_url);
      KJ_FAIL_ASSERT("should have failed");
    } catch (...) {
      auto ex = kj::getCaughtExceptionAsKj();
      KJ_EXPECT(ex.getDescription().endsWith("jsg.Error: Recursive symbolic link detected"));
    }

    try {
      // This symlink is circular. It should throw an exception.
      vfs->resolve(env.js, "file:///b"_url);
      KJ_FAIL_ASSERT("should have failed");
    } catch (...) {
      auto ex = kj::getCaughtExceptionAsKj();
      KJ_EXPECT(ex.getDescription().endsWith("jsg.Error: Recursive symbolic link detected"));
    }

    try {
      // This symlink is circular. It should throw an exception.
      vfs->resolve(env.js, "file:///c"_url);
      KJ_FAIL_ASSERT("should have failed");
    } catch (...) {
      auto ex = kj::getCaughtExceptionAsKj();
      KJ_EXPECT(ex.getDescription().endsWith("jsg.Error: Recursive symbolic link detected"));
    }

    // And while we're at it, let's check that a symlink can be removed
    KJ_EXPECT(tempDir->remove(env.js, kj::Path({"a"})));
    // Removing the symlink means that the cycle is broken and a resolve for
    // c or b should return kj::none.
    KJ_EXPECT(vfs->resolve(env.js, "file:///a"_url) == kj::none);
    KJ_EXPECT(vfs->resolve(env.js, "file:///b"_url) == kj::none);
    KJ_EXPECT(vfs->resolve(env.js, "file:///c"_url) == kj::none);
  });
}
}  // namespace
}  // namespace workerd
