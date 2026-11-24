// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-fs-rust.h"

#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <workerd/util/autogate.h>
#include <workerd/util/uuid.h>

#include <v8-microtask-queue.h>

#include <exception>

// Each class here wraps a handle of the Rust implementation in the C++ interface. The wrappers
// are made per use: two wrappers may stand for the same node, so nodes are compared with
// isSameNode(). Every call into Rust passes a Host, the C++ side of the Rust `VfsHost`, which
// keeps any exception console.log throws and rethrows it once Rust has returned, so that no C++
// exception unwinds through Rust.

namespace workerd {

namespace {

// The status codes of the bridge: a value, or nothing. Any other status is an FsError.
constexpr int32_t kOk = static_cast<int32_t>(vfs::Status::Ok);
constexpr int32_t kNone = static_cast<int32_t>(vfs::Status::None);

// The bridge passes these enums by value, so the Rust copies must have the same values.
template <typename Cpp, typename Rust>
constexpr bool sameValue(Cpp cpp, Rust rust) {
  return static_cast<int>(cpp) == static_cast<int>(rust);
}
static_assert(sameValue(FsError::NOT_DIRECTORY, vfs::FsErrorCode::NotDirectory));
static_assert(sameValue(FsError::NOT_EMPTY, vfs::FsErrorCode::NotEmpty));
static_assert(sameValue(FsError::READ_ONLY, vfs::FsErrorCode::ReadOnly));
static_assert(sameValue(FsError::NOT_PERMITTED, vfs::FsErrorCode::NotPermitted));
static_assert(
    sameValue(FsError::NOT_PERMITTED_ON_DIRECTORY, vfs::FsErrorCode::NotPermittedOnDirectory));
static_assert(sameValue(FsError::ALREADY_EXISTS, vfs::FsErrorCode::AlreadyExists));
static_assert(sameValue(FsError::TOO_MANY_OPEN_FILES, vfs::FsErrorCode::TooManyOpenFiles));
static_assert(sameValue(FsError::FAILED, vfs::FsErrorCode::Failed));
static_assert(sameValue(FsError::NOT_SUPPORTED, vfs::FsErrorCode::NotSupported));
static_assert(sameValue(FsError::INVALID_PATH, vfs::FsErrorCode::InvalidPath));
static_assert(
    sameValue(FsError::FILE_SIZE_LIMIT_EXCEEDED, vfs::FsErrorCode::FileSizeLimitExceeded));
static_assert(sameValue(FsError::SYMLINK_DEPTH_EXCEEDED, vfs::FsErrorCode::SymlinkDepthExceeded));
static_assert(sameValue(FsError::NOT_FOUND, vfs::FsErrorCode::NotFound));
static_assert(sameValue(FsType::FILE, vfs::FsTypeCode::File));
static_assert(sameValue(FsType::DIRECTORY, vfs::FsTypeCode::Directory));
static_assert(sameValue(FsType::SYMLINK, vfs::FsTypeCode::Symlink));
static_assert(sameValue(VirtualFileSystem::Stdio::IN, vfs::StdioCode::In));
static_assert(sameValue(VirtualFileSystem::Stdio::OUT, vfs::StdioCode::Out));
static_assert(sameValue(VirtualFileSystem::Stdio::ERR, vfs::StdioCode::Err));

::rust::Slice<const uint8_t> toBytes(kj::ArrayPtr<const kj::byte> bytes) {
  return {bytes.begin(), bytes.size()};
}

::rust::Slice<const uint8_t> toBytes(kj::StringPtr text) {
  return toBytes(text.asBytes());
}

kj::String fromBytes(::rust::Slice<const uint8_t> bytes) {
  return kj::heapString(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// A path's components joined by '/', which the bridge splits. Unlike PathPtr::toString(), this
// gives "" rather than "." for the empty path.
kj::String joinPath(kj::PathPtr path) {
  return kj::strArray(path, "/");
}

// A node's identifier from the bridge. It is stored in the node, like the C++ nodes' identifiers,
// so it stays valid after this call's wrapper of the node is gone.
kj::StringPtr toUniqueId(::rust::Slice<const uint8_t> id) {
  JSG_REQUIRE(id.size() > 0, Error, "Cannot generate a unique ID outside of a request");
  return kj::StringPtr(reinterpret_cast<const char*>(id.data()), id.size() - 1);
}

kj::Maybe<FsError> toError(int32_t status) {
  if (status == kOk) return kj::none;
  return static_cast<FsError>(status);
}

Stat toStat(const vfs::StatOut& stat) {
  return Stat{
    .type = static_cast<FsType>(stat.fs_type),
    .size = stat.size,
    .lastModified = kj::UNIX_EPOCH + stat.last_modified * kj::NANOSECONDS,
    .created = kj::UNIX_EPOCH + stat.created * kj::NANOSECONDS,
    .writable = stat.writable,
    .device = stat.device,
  };
}

// A name of the Rust implementation for heap snapshots, which is NUL-terminated.
kj::StringPtr toMemoryName(::rust::Slice<const uint8_t> name) {
  return kj::StringPtr(reinterpret_cast<const char*>(name.data()), name.size() - 1);
}

// Tells the Rust nodes' entries in a heap snapshot from other retainers at the same address.
const char kRustNodeType = 0;

// Reports the Rust file system's nodes to a heap snapshot. Each node is keyed by the address of
// its shared state, which lives as long as the node, as a C++ node's address does; the wrappers
// worker-fs.h's callers hold are nodes of their own, each with an edge to the node it wraps.
class CppSnapshotTracker final: public vfs::SnapshotTracker {
 public:
  explicit CppSnapshotTracker(jsg::MemoryTracker& tracker): tracker(tracker) {}

  bool pushNode(::rust::Slice<const uint8_t> edge,
      size_t id,
      ::rust::Slice<const uint8_t> name,
      size_t selfSize) override {
    return tracker.pushRetainer(reinterpret_cast<const void*>(id), &kRustNodeType,
        toMemoryName(name), selfSize, toMemoryName(edge));
  }

  void popNode() override {
    tracker.popNode();
  }

  void trackBytes(
      ::rust::Slice<const uint8_t> edge, ::rust::Slice<const uint8_t> name, size_t size) override {
    tracker.trackFieldWithSize(toMemoryName(edge), size, toMemoryName(name));
  }

 private:
  jsg::MemoryTracker& tracker;
};

kj::Path toPath(::rust::Vec<uint8_t> joined) {
  if (joined.empty()) return kj::Path(nullptr);
  return kj::Path::parse(kj::str(
      kj::ArrayPtr<const char>(reinterpret_cast<const char*>(joined.data()), joined.size())));
}

class Host final: public vfs::VfsHost {
 public:
  explicit Host(jsg::Lock& js): js(js), previous(current) {
    current = this;
  }
  ~Host() noexcept(false) {
    current = previous;
  }
  KJ_DISALLOW_COPY_AND_MOVE(Host);

  // The host of the innermost call into the file system on this thread, for C++ that the file
  // system calls without one, such as a lazy directory's loader.
  static kj::Maybe<Host&> tryGetCurrent() {
    return current;
  }

  // Keeps what went wrong, to throw once the file system returns. The first failure wins.
  void fail(std::exception_ptr exception) const {
    if (!failure) {
      failure = kj::mv(exception);
    }
  }

  // Throws what went wrong while the file system ran, such as an exception from console.log or a
  // lazy directory's loader, if anything.
  void throwIfFailed() {
    if (failure) {
      std::rethrow_exception(kj::mv(failure));
    }
  }

  size_t isolate() const override {
    return reinterpret_cast<size_t>(js.v8Isolate);
  }

  size_t blobSizeLimit() const override {
    return Worker::Isolate::from(js).getLimitEnforcer().getBlobSizeLimit();
  }

  bool fillRandom(::rust::Slice<uint8_t> buffer) const override {
    KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
      ioContext.getEntropySource().generate(kj::arrayPtr(buffer.data(), buffer.size()));
      return true;
    }
    return false;
  }

  ::rust::String randomUuid() const override {
    KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
      auto uuid = workerd::randomUUID(ioContext.getEntropySource());
      return ::rust::String(uuid.begin(), uuid.size());
    }
    return ::rust::String();
  }

  bool hasIoContext() const override {
    return IoContext::hasCurrent();
  }

  bool logStdio(uint8_t stdio, ::rust::Slice<const uint8_t> line) const override {
    try {
      writeStdio(
          js, static_cast<VirtualFileSystem::Stdio>(stdio), kj::arrayPtr(line.data(), line.size()));
      return true;
    } catch (...) {
      fail(std::current_exception());
      return false;
    }
  }

  void enqueueMicrotask(::rust::Box<vfs::RsMicrotask> task) const override;

  bool hasIoContextTmpStore() const override;
  ::rust::Box<vfs::RsTmpStore> ioContextTmpStore() const override;

 private:
  static thread_local kj::Maybe<Host&> current;

  jsg::Lock& js;
  kj::Maybe<Host&> previous;
  mutable std::exception_ptr failure;
};

thread_local kj::Maybe<Host&> Host::current = kj::none;

// Gives an exception from C++ that Rust called to the host of the running call, to throw once
// the file system returns. Without one, as when a wrapped descriptor is closed by its handle's
// destructor, there is nowhere to throw it, so it is logged.
void keepOrLog(std::exception_ptr exception) {
  KJ_IF_SOME(host, Host::tryGetCurrent()) {
    host.fail(kj::mv(exception));
    return;
  }
  try {
    std::rethrow_exception(kj::mv(exception));
  } catch (...) {
    KJ_LOG(ERROR, "the Rust file system called C++ that threw outside any call",
        kj::getCaughtExceptionAsKj());
  }
}

// Runs func with a Host, then throws what the host kept, if anything.
template <typename Func>
auto withHost(jsg::Lock& js, Func&& func) {
  Host host(js);
  if constexpr (kj::isSameType<decltype(func(host)), void>()) {
    func(host);
    host.throwIfFailed();
  } else {
    auto result = func(host);
    host.throwIfFailed();
    return result;
  }
}

}  // namespace

class RustFile final: public File {
 public:
  explicit RustFile(::rust::Box<vfs::RsFile> inner): inner(kj::mv(inner)) {}
  KJ_DISALLOW_COPY_AND_MOVE(RustFile);

  const vfs::RsFile& get() const {
    return *inner;
  }

  kj::Maybe<const RustFile&> tryGetRust() const override {
    return *this;
  }

  kj::Maybe<FsError> setLastModified(jsg::Lock& js, kj::Date date) override {
    return toError(vfs::file_set_last_modified(*inner, (date - kj::UNIX_EPOCH) / kj::NANOSECONDS));
  }

  Stat stat(jsg::Lock& js) override {
    return toStat(vfs::file_stat(*inner));
  }

  uint32_t read(jsg::Lock& js, uint32_t offset, kj::ArrayPtr<kj::byte> buffer) const override {
    return withHost(js, [&](Host& host) {
      return vfs::file_read(*inner, host, offset, {buffer.begin(), buffer.size()});
    });
  }

  kj::OneOf<FsError, uint32_t> write(
      jsg::Lock& js, uint32_t offset, kj::ArrayPtr<const kj::byte> data) override {
    auto result = withHost(
        js, [&](Host& host) { return vfs::file_write(*inner, host, offset, toBytes(data)); });
    if (result.status == kOk) return result.value;
    return static_cast<FsError>(result.status);
  }

  kj::Maybe<FsError> fill(jsg::Lock& js, kj::byte val, kj::Maybe<uint32_t> offset) override {
    return toError(vfs::file_fill(*inner, val, offset.orDefault(0), offset != kj::none));
  }

  kj::Maybe<FsError> resize(jsg::Lock& js, uint32_t size) override {
    return toError(withHost(js, [&](Host& host) { return vfs::file_resize(*inner, host, size); }));
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "RustFile"_kj;
  }

  size_t jsgGetMemorySelfSize() const override {
    return sizeof(RustFile);
  }

  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    CppSnapshotTracker rustTracker(tracker);
    vfs::file_track_memory(*inner, rustTracker);
  }

  kj::OneOf<FsError, kj::Rc<File>> clone(jsg::Lock& js) override;

  kj::Maybe<FsError> replace(jsg::Lock& js, kj::Rc<File> file) override;

  kj::StringPtr getUniqueId(jsg::Lock& js) const override {
    return toUniqueId(withHost(js, [&](Host& host) { return vfs::file_unique_id(*inner, host); }));
  }

  bool isSameNode(const File& other) const override {
    KJ_IF_SOME(rust, other.tryGetRust()) {
      return vfs::file_is_same(*inner, rust.get());
    }
    return false;
  }

  void countTowardsIsolateLimit(jsg::Lock& js) const override {
    withHost(js, [&](Host& host) { vfs::file_count_towards_isolate_limit(*inner, host); });
  }

 private:
  ::rust::Box<vfs::RsFile> inner;
};

class RustDirectory final: public Directory {
 public:
  explicit RustDirectory(::rust::Box<vfs::RsDirectory> inner): inner(kj::mv(inner)) {}
  KJ_DISALLOW_COPY_AND_MOVE(RustDirectory);

  const vfs::RsDirectory& get() const {
    return *inner;
  }

  kj::Maybe<const RustDirectory&> tryGetRust() const override {
    return *this;
  }

  kj::Maybe<kj::OneOf<FsError, Stat>> stat(jsg::Lock& js, kj::PathPtr path) override {
    auto joined = joinPath(path);
    auto result =
        withHost(js, [&](Host& host) { return vfs::dir_stat(*inner, host, toBytes(joined)); });
    if (result.status == kNone) return kj::none;
    if (result.status == kOk) return kj::OneOf<FsError, Stat>(toStat(result.stat));
    return kj::OneOf<FsError, Stat>(static_cast<FsError>(result.status));
  }

  size_t count(jsg::Lock& js, kj::Maybe<FsType> typeFilter) override {
    auto filter = typeFilter.map([](FsType type) { return static_cast<int32_t>(type); });
    return withHost(
        js, [&](Host& host) { return vfs::dir_count(*inner, host, filter.orDefault(-1)); });
  }

  kj::Array<ListEntry> list(jsg::Lock& js) override;

  kj::Maybe<FsNodeWithError> tryOpen(jsg::Lock& js, kj::PathPtr path, OpenOptions options) override;

  kj::Maybe<FsError> add(jsg::Lock& js, kj::StringPtr name, Item item) override;

  kj::OneOf<FsError, bool> remove(jsg::Lock& js, kj::PathPtr path, RemoveOptions options) override {
    auto joined = joinPath(path);
    auto result = withHost(js, [&](Host& host) {
      return vfs::dir_remove(*inner, host, toBytes(joined), options.recursive);
    });
    if (result.status == kOk) return result.value != 0;
    return static_cast<FsError>(result.status);
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "RustDirectory"_kj;
  }

  size_t jsgGetMemorySelfSize() const override {
    return sizeof(RustDirectory);
  }

  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    CppSnapshotTracker rustTracker(tracker);
    vfs::dir_track_memory(*inner, rustTracker);
  }

  kj::StringPtr getUniqueId(jsg::Lock& js) const override {
    return toUniqueId(withHost(js, [&](Host& host) { return vfs::dir_unique_id(*inner, host); }));
  }

  bool isSameNode(const Directory& other) const override {
    KJ_IF_SOME(rust, other.tryGetRust()) {
      return vfs::dir_is_same(*inner, rust.get());
    }
    return false;
  }

  void countTowardsIsolateLimit(jsg::Lock& js) const override {
    withHost(js, [&](Host& host) { vfs::dir_count_towards_isolate_limit(*inner, host); });
  }

 private:
  ::rust::Box<vfs::RsDirectory> inner;
};

namespace {

const vfs::RsFile& getRust(const File& file) {
  KJ_IF_SOME(rust, file.tryGetRust()) {
    return rust.get();
  }
  KJ_FAIL_REQUIRE("a file of the C++ file system can't be used in the Rust one");
}

const vfs::RsDirectory& getRust(const Directory& dir) {
  KJ_IF_SOME(rust, dir.tryGetRust()) {
    return rust.get();
  }
  KJ_FAIL_REQUIRE("a directory of the C++ file system can't be used in the Rust one");
}

const vfs::RsSymbolicLink& getRust(const SymbolicLink& link) {
  KJ_IF_SOME(rust, link.tryGetRust()) {
    return rust.get();
  }
  KJ_FAIL_REQUIRE("a symbolic link of the C++ file system can't be used in the Rust one");
}

kj::Rc<File> wrap(::rust::Box<vfs::RsFile> file) {
  return kj::rc<RustFile>(kj::mv(file));
}

kj::Rc<Directory> wrap(::rust::Box<vfs::RsDirectory> dir) {
  return kj::rc<RustDirectory>(kj::mv(dir));
}

kj::Rc<SymbolicLink> wrap(::rust::Box<vfs::RsSymbolicLink> link) {
  auto target = toPath(vfs::link_target(*link));
  return kj::rc<SymbolicLink>(kj::heap<RustSymbolicLink>(kj::mv(link)), kj::mv(target));
}

kj::Maybe<FsNodeWithError> toNode(::rust::Box<vfs::RsNode> node) {
  switch (vfs::node_kind(*node)) {
    case vfs::NodeKind::None:
      return kj::none;
    case vfs::NodeKind::Error:
      return FsNodeWithError(static_cast<FsError>(vfs::node_error(*node)));
    case vfs::NodeKind::File:
      return FsNodeWithError(wrap(vfs::node_into_file(kj::mv(node))));
    case vfs::NodeKind::Directory:
      return FsNodeWithError(wrap(vfs::node_into_directory(kj::mv(node))));
    case vfs::NodeKind::SymbolicLink:
      return FsNodeWithError(wrap(vfs::node_into_symbolic_link(kj::mv(node))));
  }
  KJ_UNREACHABLE;
}

// A node that is known to be there, such as an entry of a listing.
FsNode toItem(::rust::Box<vfs::RsNode> node) {
  KJ_SWITCH_ONEOF(KJ_ASSERT_NONNULL(toNode(kj::mv(node)))) {
    KJ_CASE_ONEOF(file, kj::Rc<File>) {
      return kj::mv(file);
    }
    KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
      return kj::mv(dir);
    }
    KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
      return kj::mv(link);
    }
    KJ_CASE_ONEOF(error, FsError) {
      KJ_FAIL_ASSERT("expected a node, not an error", static_cast<int>(error));
    }
  }
  KJ_UNREACHABLE;
}

::rust::Box<vfs::RsNode> toRust(const Directory::Item& item) {
  KJ_SWITCH_ONEOF(item) {
    KJ_CASE_ONEOF(file, kj::Rc<File>) {
      return vfs::node_from_file(getRust(*file));
    }
    KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
      return vfs::node_from_directory(getRust(*dir));
    }
    KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
      return vfs::node_from_symbolic_link(getRust(*link));
    }
  }
  KJ_UNREACHABLE;
}

void Host::enqueueMicrotask(::rust::Box<vfs::RsMicrotask> task) const {
  // The callback can't move out of what it captures, so the task is kept where it can.
  struct Task {
    mutable kj::Maybe<::rust::Box<vfs::RsMicrotask>> task;
  };
  // Making the callback can throw, as on a terminating isolate; the exception is thrown once the
  // file system returns.
  try {
    auto callback = js.wrapSimpleFunction(js.v8Context(),
        [task = Task{kj::mv(task)}](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>&) {
      KJ_IF_SOME(pending, task.task) {
        ::rust::Box<vfs::RsMicrotask> run = kj::mv(pending);
        task.task = kj::none;
        withHost(js, [&](Host& host) { vfs::run_microtask(kj::mv(run), host); });
      }
    }, jsg::Once::YES);
    js.v8Context()->GetMicrotaskQueue()->EnqueueMicrotask(js.v8Isolate, callback);
  } catch (...) {
    fail(std::current_exception());
  }
}

bool Host::hasIoContextTmpStore() const {
  KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
    return ioContext.getTmpDirStoreScope().tryGetRust() != kj::none;
  }
  return false;
}

::rust::Box<vfs::RsTmpStore> Host::ioContextTmpStore() const {
  auto& store = KJ_ASSERT_NONNULL(IoContext::current().getTmpDirStoreScope().tryGetRust());
  return vfs::tmp_store_add_ref(store.get());
}

// VirtualFileSystem::Observer for the Rust file system.
// An exception from the observer is thrown once the file system returns, as the C++
// implementation throws it from the operation.
class CppObserver final: public vfs::FdObserver {
 public:
  explicit CppObserver(kj::Own<VirtualFileSystem::Observer> inner): inner(kj::mv(inner)) {}

  void onOpen(size_t openFds, size_t totalFds) const override {
    guard([&] { inner->onOpen(openFds, totalFds); });
  }

  void onClose(size_t openFds, size_t totalFds) const override {
    guard([&] { inner->onClose(openFds, totalFds); });
  }

  void onMaxFds(size_t openFds) const override {
    guard([&] { inner->onMaxFds(openFds); });
  }

 private:
  template <typename Func>
  static void guard(Func&& func) {
    try {
      func();
    } catch (...) {
      keepOrLog(std::current_exception());
    }
  }

  kj::Own<VirtualFileSystem::Observer> inner;
};

// A kj::Array kept by a read-only file of the Rust file system.
class OwnedArray final: public vfs::OwnedBytes {
 public:
  explicit OwnedArray(kj::Array<const kj::byte> data): data(kj::mv(data)) {}

  ::rust::Slice<const uint8_t> bytes() const override {
    return toBytes(data);
  }

 private:
  kj::Array<const kj::byte> data;
};

class CppLazyLoader final: public vfs::LazyLoader {
 public:
  explicit CppLazyLoader(kj::Function<kj::Rc<Directory>()> func): func(kj::mv(func)) {}

  // An exception can't unwind through Rust, so the host keeps it and throws it once the file
  // system returns: from the operation that loaded the directory, as in C++. The function stays,
  // to be called again on the next use, as in C++.
  bool load(vfs::RsLoaded& loaded) override {
    try {
      auto dir = func();
      auto& rust = KJ_REQUIRE_NONNULL(
          dir->tryGetRust(), "a lazy directory of the Rust file system loaded a C++ directory");
      vfs::loaded_set(loaded, vfs::dir_add_ref(rust.get()));
      return true;
    } catch (...) {
      keepOrLog(std::current_exception());
      return false;
    }
  }

 private:
  kj::Function<kj::Rc<Directory>()> func;
};

class RustOpenedFile final: public VirtualFileSystem::OpenedFile {
 public:
  RustOpenedFile(::rust::Box<vfs::RsOpenedFile> inner, FsNode node)
      : OpenedFile(vfs::opened_fd(*inner),
            vfs::opened_read(*inner),
            vfs::opened_write(*inner),
            vfs::opened_append(*inner),
            kj::mv(node)),
        inner(kj::mv(inner)) {}

  uint32_t getPosition() const override {
    return vfs::opened_position(*inner);
  }

  void setPosition(uint32_t newPosition) override {
    vfs::opened_set_position(*inner, newPosition);
  }

 private:
  ::rust::Box<vfs::RsOpenedFile> inner;
};

kj::Rc<VirtualFileSystem::OpenedFile> wrap(::rust::Box<vfs::RsOpenedFile> opened) {
  auto node = toItem(vfs::opened_node(*opened));
  return kj::rc<RustOpenedFile>(kj::mv(opened), kj::mv(node));
}

// Holds a wrapped file descriptor or a lock of the Rust file system.
class Handle final {
 public:
  explicit Handle(::rust::Box<vfs::RsHandle> inner): inner(kj::mv(inner)) {}

 private:
  ::rust::Box<vfs::RsHandle> inner;
};

class RustVirtualFileSystem final: public VirtualFileSystem {
 public:
  RustVirtualFileSystem(kj::Own<FsMap> fsMap, ::rust::Box<vfs::RsVfs> inner)
      : fsMap(kj::mv(fsMap)),
        inner(kj::mv(inner)) {}

  kj::Rc<Directory> getRoot(jsg::Lock& js) const override {
    return wrap(vfs::vfs_root(*inner));
  }

  const jsg::Url& getBundleRoot() const override {
    return fsMap->getBundleRoot();
  }

  const jsg::Url& getTmpRoot() const override {
    return fsMap->getTempRoot();
  }

  const jsg::Url& getDevRoot() const override {
    return fsMap->getDevRoot();
  }

  kj::Rc<OpenedFile> getStdio(jsg::Lock& js, Stdio stdio) const override {
    return wrap(vfs::vfs_get_stdio(*inner, static_cast<uint8_t>(stdio)));
  }

  kj::OneOf<FsError, kj::Rc<OpenedFile>> openFd(
      jsg::Lock& js, const jsg::Url& url, OpenOptions options) const override {
    kj::Path root{};
    auto path = joinPath(root.eval(kj::str(url.getPathname().slice(1))));
    vfs::FdOpenOptions rustOptions{
      .read = options.read,
      .write = options.write,
      .append = options.append,
      .create = options.create,
      .exclusive = options.exclusive,
      .truncate = options.truncate,
      .follow_links = options.followLinks,
    };
    auto opened = withHost(
        js, [&](Host& host) { return vfs::vfs_open_fd(*inner, host, toBytes(path), rustOptions); });
    auto status = vfs::opened_status(*opened);
    if (status != kOk) return static_cast<FsError>(status);
    return wrap(kj::mv(opened));
  }

  void closeFd(jsg::Lock& js, int fd) const override {
    // Under a host, so that an exception from the observer is thrown from here.
    withHost(js, [&](Host&) { vfs::vfs_close_fd(*inner, fd); });
  }

  kj::Own<void> wrapFd(jsg::Lock& js, int fd) const override {
    return kj::heap<Handle>(vfs::vfs_wrap_fd(*inner, fd));
  }

  kj::Maybe<kj::Rc<OpenedFile>> tryGetFd(jsg::Lock& js, int fd) const override {
    auto opened = vfs::vfs_try_get_fd(*inner, fd);
    if (vfs::opened_status(*opened) != kOk) return kj::none;
    return wrap(kj::mv(opened));
  }

  // Per the web file system spec, a directory can't be removed while any of its descendants is
  // locked, so locking a locator locks its parents too.
  kj::Own<void> lock(jsg::Lock& js, const jsg::Url& locator) const override {
    ::rust::Vec<::rust::String> locators;
    auto add = [&](const jsg::Url& url) {
      auto href = url.getHref();
      locators.push_back(::rust::String(href.begin(), href.size()));
    };
    add(locator);
    auto maybeParent = locator.getParent();
    while (maybeParent != kj::none) {
      auto& parent = KJ_ASSERT_NONNULL(maybeParent);
      add(parent);
      maybeParent = parent.getParent();
    }
    return kj::heap<Handle>(vfs::vfs_lock(*inner, kj::mv(locators)));
  }

  bool isLocked(jsg::Lock& js, const jsg::Url& locator) const override {
    auto href = locator.getHref();
    return vfs::vfs_is_locked(*inner, ::rust::Str(href.begin(), href.size()));
  }

 private:
  kj::Own<FsMap> fsMap;
  ::rust::Box<vfs::RsVfs> inner;
};

}  // namespace

kj::OneOf<FsError, kj::Rc<File>> RustFile::clone(jsg::Lock& js) {
  auto result = withHost(js, [&](Host& host) { return vfs::file_clone(*inner, host); });
  KJ_SWITCH_ONEOF(KJ_ASSERT_NONNULL(toNode(kj::mv(result)))) {
    KJ_CASE_ONEOF(file, kj::Rc<File>) {
      return kj::mv(file);
    }
    KJ_CASE_ONEOF(error, FsError) {
      return error;
    }
    KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
      KJ_UNREACHABLE;
    }
    KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
      KJ_UNREACHABLE;
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<FsError> RustFile::replace(jsg::Lock& js, kj::Rc<File> file) {
  auto& other = getRust(*file);
  return toError(withHost(js, [&](Host& host) { return vfs::file_replace(*inner, host, other); }));
}

kj::Array<Directory::ListEntry> RustDirectory::list(jsg::Lock& js) {
  auto listing = withHost(js, [&](Host& host) { return vfs::dir_list(*inner, host); });
  auto count = vfs::listing_len(*listing);
  auto entries = kj::heapArrayBuilder<ListEntry>(count);
  for (size_t i = 0; i < count; i++) {
    entries.add(ListEntry{
      .name = fromBytes(vfs::listing_name(*listing, i)),
      .item = toItem(vfs::listing_node(*listing, i)),
    });
  }
  return entries.finish();
}

kj::Maybe<FsNodeWithError> RustDirectory::tryOpen(
    jsg::Lock& js, kj::PathPtr path, OpenOptions options) {
  auto joined = joinPath(path);
  auto createAs = options.createAs.map([](FsType type) { return static_cast<int32_t>(type); });
  return toNode(withHost(js, [&](Host& host) {
    return vfs::dir_try_open(
        *inner, host, toBytes(joined), createAs.orDefault(-1), options.followLinks);
  }));
}

kj::Maybe<FsError> RustDirectory::add(jsg::Lock& js, kj::StringPtr name, Item item) {
  auto node = toRust(item);
  return toError(withHost(
      js, [&](Host& host) { return vfs::dir_add(*inner, host, toBytes(name), kj::mv(node)); }));
}

namespace vfs {

bool isEnabled() {
  return util::Autogate::isEnabled(util::AutogateKey::RUST_VFS);
}

kj::Rc<File> newWritableFile(jsg::Lock& js, kj::Maybe<uint32_t> size) {
  return wrap(withHost(js,
      [&](Host& host) { return file_new_writable(host, size.orDefault(0), size != kj::none); }));
}

kj::Rc<File> newReadableFile(kj::ArrayPtr<const kj::byte> data) {
  // The caller keeps the data alive for as long as the file, as File::newReadable() requires.
  return wrap(file_new_readable_view(data.begin(), data.size()));
}

kj::Rc<File> newReadableFile(kj::Array<const kj::byte> data) {
  return wrap(file_new_readable_owned(kj::heap<OwnedArray>(kj::mv(data))));
}

kj::Rc<File> newDevNull() {
  return wrap(file_new_dev_null());
}

kj::Rc<File> newDevZero() {
  return wrap(file_new_dev_zero());
}

kj::Rc<File> newDevFull() {
  return wrap(file_new_dev_full());
}

kj::Rc<File> newDevRandom() {
  return wrap(file_new_dev_random());
}

kj::Rc<Directory> newWritableDirectory() {
  return wrap(dir_new_writable());
}

kj::Rc<Directory> newWritableDirectory(jsg::Lock& js) {
  return wrap(withHost(js, [&](Host& host) { return dir_new_writable_counted(host); }));
}

kj::Rc<Directory> newTmpDirectory() {
  return wrap(dir_new_tmp());
}

kj::Rc<Directory> newLazyDirectory(kj::Function<kj::Rc<Directory>()> func) {
  return wrap(dir_new_lazy(kj::heap<CppLazyLoader>(kj::mv(func))));
}

kj::Rc<Directory> finishBuilder(Directory::Builder::Map entries) {
  auto builder = builder_new();
  for (auto& entry: entries) {
    auto name = toBytes(entry.key);
    KJ_SWITCH_ONEOF(entry.value) {
      KJ_CASE_ONEOF(file, kj::Rc<File>) {
        builder_add(*builder, name, node_from_file(getRust(*file)));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
        builder_add(*builder, name, node_from_directory(getRust(*dir)));
      }
      KJ_CASE_ONEOF(nested, kj::Own<Directory::Builder>) {
        auto dir = nested->finish();
        builder_add(*builder, name, node_from_directory(getRust(*dir)));
      }
    }
  }
  return wrap(builder_finish(kj::mv(builder)));
}

kj::Own<VirtualFileSystem> newVirtualFileSystem(
    kj::Own<FsMap> fsMap, kj::Rc<Directory> root, kj::Own<VirtualFileSystem::Observer> observer) {
  auto inner = vfs_new(getRust(*root), kj::heap<CppObserver>(kj::mv(observer)));
  return kj::heap<RustVirtualFileSystem>(kj::mv(fsMap), kj::mv(inner));
}

kj::Maybe<kj::Own<RustSymbolicLink>> tryNewSymbolicLink(
    const Directory& root, kj::PathPtr target, kj::Maybe<kj::StringPtr> rawTarget) {
  KJ_IF_SOME(rust, root.tryGetRust()) {
    auto joined = joinPath(target);
    RawTarget raw{.kept = rawTarget != kj::none};
    KJ_IF_SOME(text, rawTarget) {
      raw.target.reserve(text.size());
      for (auto byte: text.asBytes()) raw.target.push_back(byte);
    }
    return kj::heap<RustSymbolicLink>(link_new(rust.get(), toBytes(joined), raw));
  }
  return kj::none;
}

kj::Maybe<kj::String> getLinkRawTarget(const RustSymbolicLink& link) {
  auto raw = link_raw_target(link.get());
  if (!raw.kept) return kj::none;
  return kj::heapString(reinterpret_cast<const char*>(raw.target.data()), raw.target.size());
}

kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>> resolveLink(
    jsg::Lock& js, const RustSymbolicLink& link) {
  auto resolved = withHost(js, [&](Host& host) {
    KJ_IF_SOME(guard, SymbolicLinkRecursionGuardScope::tryGetRustGuard()) {
      return guard.lend(
          [&](vfs::RsLinkGuard& inner) { return link_resolve_in(link.get(), host, inner); });
    }
    return link_resolve(link.get(), host);
  });
  KJ_IF_SOME(node, toNode(kj::mv(resolved))) {
    KJ_SWITCH_ONEOF(node) {
      KJ_CASE_ONEOF(file, kj::Rc<File>) {
        return kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>(kj::mv(file));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
        return kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>(kj::mv(dir));
      }
      KJ_CASE_ONEOF(error, FsError) {
        return kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>(error);
      }
      KJ_CASE_ONEOF(nested, kj::Rc<SymbolicLink>) {
        // Resolution follows every link.
        KJ_UNREACHABLE;
      }
    }
  }
  return kj::none;
}

kj::StringPtr getLinkUniqueId(jsg::Lock& js, const RustSymbolicLink& link) {
  return toUniqueId(withHost(js, [&](Host& host) { return link_unique_id(link.get(), host); }));
}

void countLinkTowardsIsolateLimit(jsg::Lock& js, const RustSymbolicLink& link) {
  withHost(js, [&](Host& host) { link_count_towards_isolate_limit(link.get(), host); });
}

bool isSameLink(const RustSymbolicLink& link, const RustSymbolicLink& other) {
  return link_is_same(link.get(), other.get());
}

kj::Own<RustLinkGuard> newLinkGuard() {
  return kj::heap<RustLinkGuard>();
}

kj::Maybe<FsError> checkLink(RustLinkGuard& guard, const RustSymbolicLink& link) {
  return guard.lend(
      [&](vfs::RsLinkGuard& inner) { return toError(link_guard_check(inner, link.get())); });
}

kj::Own<RustTmpStore> newTmpStore(MakeCurrent makeCurrent) {
  if (makeCurrent) {
    auto scope = tmp_scope_enter();
    auto store = tmp_scope_store(*scope);
    return kj::heap<RustTmpStore>(kj::mv(store), kj::mv(scope));
  }
  return kj::heap<RustTmpStore>(tmp_store_new(), kj::none);
}

kj::Rc<Directory> getTmpStoreDirectory(const RustTmpStore& store) {
  return wrap(tmp_store_directory(store.get()));
}

}  // namespace vfs
}  // namespace workerd
