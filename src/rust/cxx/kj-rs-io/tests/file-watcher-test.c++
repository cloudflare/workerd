// Tests for kj-rs-io's file watcher (watcher.rs, through the bridge), the tokio-loop replacement for workerd's
// --watch FileWatcher (Rust, over the notify crate: watcher.rs). Exercises the behaviors workerd
// depends on: plain modification, atomic replace-by-rename (editor saves), event
// queueing/coalescing across onChange() calls, the already-open-fd watch() signature,
// missing-file handling, and teardown/cancel while a watch promise is armed.

#include "kj-rs-io/async-io.h"
#include "kj-rs-io/ffi.rs.h"

#include <kj/async.h>
#include <kj/debug.h>
#include <kj/filesystem.h>
#include <kj/test.h>

#include <cstdlib>
#include <cstring>

#if !_WIN32
#include <sys/stat.h>  // chmod(2): a file operation kj::Filesystem does not expose
#include <unistd.h>    // getpid(), for unique scratch-directory names; symlink(2)
#endif

namespace kj_rs_io_test {
namespace {

using kj_rs_io::setupTokioAsyncIo;
using kj_rs_io::TokioAsyncIoContext;

// The three bridged watcher calls behind workerd's TokioFileWatcher (server/cli-io-backend.c++),
// wrapped the same way it wraps them: paths cross as the bytes kj::Path::toNativeString produces
// (a unix path need not be UTF-8), and onChange() is started inside the call (async-io.h,
// operation-start policy) so a caller that merely retains the promise still has its files
// watched.
class FileWatcher {
 public:
  FileWatcher(): inner(kj_rs_io::new_file_watcher()) {}
  KJ_DISALLOW_COPY_AND_MOVE(FileWatcher);

  void watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile &> = kj::none) {
    auto native = path.toNativeString(true);
    kj_rs_io::file_watcher_watch(*inner,
        ::rust::Slice<const uint8_t>(
            reinterpret_cast<const uint8_t *>(native.begin()), native.size()));
  }

  kj::Promise<void> onChange() {
    return kj_rs_io::file_watcher_on_change(*inner).eagerlyEvaluate(nullptr);
  }

 private:
  ::rust::Box<kj_rs_io::TokioFileWatcher> inner;
};

#if !_WIN32

// =======================================================================================
// Helpers

// Waits for `promise` to resolve, returning true, or false after `timeout`.
bool resolvesWithin(kj::Promise<void> promise, TokioAsyncIoContext &io, kj::Duration timeout) {
  auto timedOut = io.getTimer().afterDelay(timeout).then([]() { return false; });
  return promise.then([]() { return true; })
      .exclusiveJoin(kj::mv(timedOut))
      .wait(io.getWaitScope());
}

// Generous bound for "the change fires"; file events are near-immediate on both backends.
constexpr kj::Duration FIRE_TIMEOUT = 5 * kj::SECONDS;
// Short bound for "nothing fires" checks.
constexpr kj::Duration QUIET_TIMEOUT = 200 * kj::MILLISECONDS;

// A scratch directory under TEST_TMPDIR (or /tmp), managed through kj::Filesystem: created on
// construction, removed recursively on destruction. File manipulation goes through the same
// kj::Directory, so the tests use no raw POSIX file calls -- except chmod(2), which
// kj::Filesystem does not expose.
struct TempDir {
  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  kj::Path path;  // absolute (root-relative) path of the directory
  kj::Own<const kj::Directory> dir;

  TempDir()
      : path(freshPath()),
        dir(fs->getRoot().openSubdir(path, kj::WriteMode::CREATE | kj::WriteMode::MODIFY)) {}

  static kj::Path freshPath() {
    const char *base = getenv("TEST_TMPDIR");
    if (base == nullptr) base = "/tmp";
    static uint counter = 0;
    return kj::Path::parse(kj::StringPtr(base).slice(1))
        .append(kj::str("kj-rs-io-file-watcher-test.", getpid(), ".", counter++));
  }

  ~TempDir() noexcept(false) {
    // Recursive; TEST_TMPDIR is wiped by bazel anyway.
    fs->getRoot().tryRemove(path);
  }

  kj::Path filePath(kj::StringPtr name) {
    return path.append(name);
  }

  // The absolute native path of `name`, for the one raw syscall (chmod) the tests still make.
  kj::String fileName(kj::StringPtr name) {
    return filePath(name).toString(true);
  }

  // Creates or truncates `name` with `content`.
  void writeFile(kj::StringPtr name, kj::StringPtr content) {
    dir->openFile(kj::Path(name), kj::WriteMode::CREATE | kj::WriteMode::MODIFY)
        ->writeAll(content.asBytes());
  }

  void appendFile(kj::StringPtr name, kj::StringPtr content) {
    auto file = dir->openFile(kj::Path(name), kj::WriteMode::MODIFY);
    file->write(file->stat().size, content.asBytes());
  }

  kj::Own<const kj::ReadableFile> openFile(kj::StringPtr name) {
    return dir->openFile(kj::Path(name));
  }

  // rename(2) `from` over `to`.
  void rename(kj::StringPtr from, kj::StringPtr to) {
    dir->transfer(kj::Path(to), kj::WriteMode::MODIFY, kj::Path(from), kj::TransferMode::MOVE);
  }

  void unlink(kj::StringPtr name) {
    dir->remove(kj::Path(name));
  }
};

// After a change fired, drains any further already-queued events so the next onChange() call
// starts from a quiet state (mirrors what workerd's waitForChanges() settle loop achieves).
void drain(FileWatcher &watcher, TokioAsyncIoContext &io) {
  while (resolvesWithin(watcher.onChange(), io, QUIET_TIMEOUT)) {}
}

// =======================================================================================
// Tests

KJ_TEST("FileWatcher: constructs on this platform") {
  auto io = setupTokioAsyncIo();
  FileWatcher watcher;
}

KJ_TEST("FileWatcher: modification fires onChange") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  auto change = watcher.onChange();
  dir.appendFile("a.txt", " two");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: change before onChange() is called is not lost") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  // Modify before anyone is waiting: the event queues in the kernel.
  dir.appendFile("a.txt", " two");
  KJ_EXPECT(resolvesWithin(watcher.onChange(), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: atomic replace-by-rename fires onChange") {
  // Editors typically save by writing a temporary file and rename(2)ing it over the target.
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  auto change = watcher.onChange();
  dir.writeFile("a.txt.tmp", "two");
  dir.rename("a.txt.tmp", "a.txt");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: watching via an already-open file handle") {
  // workerd passes the config files' already-open kj::ReadableFile to watch(); watching is by
  // name, so the handle is accepted and ignored, and may be closed right away.
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "one");

  auto file = dir.openFile("a.txt");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), *file);
  file = nullptr;  // The original handle may be closed; the watch must survive.

  auto change = watcher.onChange();
  dir.appendFile("a.txt", " two");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: rapid changes coalesce; watcher stays armed for later "
        "changes") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  // A burst of changes produces one resolution per onChange() call (not one per event), ...
  auto change = watcher.onChange();
  dir.appendFile("a.txt", " two");
  dir.appendFile("a.txt", " three");
  dir.appendFile("a.txt", " four");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));

  // ... and once the queue is drained, the watcher is quiet ...
  drain(watcher, io);

  // ... but still armed: a fresh change fires a fresh onChange().
  auto later = watcher.onChange();
  dir.appendFile("a.txt", " five");
  KJ_EXPECT(resolvesWithin(kj::mv(later), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: deleting the watched file fires; a recreated file is tracked where the "
        "backend can") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  // Delete: reported through the directory watch on every backend.
  auto change = watcher.onChange();
  dir.unlink("a.txt");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
  drain(watcher, io);

  // Recreate and modify: watched by name through the directory, so the new file is picked up
  // (workerd's native kqueue watcher watched the deleted inode's fd and could not).
  auto later = watcher.onChange();
  dir.writeFile("a.txt", "two");
  dir.appendFile("a.txt", " three");
  KJ_EXPECT(resolvesWithin(kj::mv(later), io, FIRE_TIMEOUT));
  drain(watcher, io);
  // ...and subsequent writes to the recreated file keep firing.
  auto again = watcher.onChange();
  dir.appendFile("a.txt", " four");
  KJ_EXPECT(resolvesWithin(kj::mv(again), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: the onChange() promise outlives the FileWatcher object") {
  // The promise co-owns the watcher state, so destroying the FileWatcher first must neither
  // crash nor invalidate the pending promise: a change made afterwards still resolves it.
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "one");

  kj::Promise<void> change = nullptr;
  {
    FileWatcher watcher;
    watcher.watch(dir.filePath("a.txt"), kj::none);
    change = watcher.onChange();
    KJ_EXPECT(!change.poll(io.getWaitScope()));
  }
  dir.appendFile("a.txt", " two");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: unrelated files in the same directory do not fire (basename filtering)") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "one");
  dir.writeFile("other.txt", "other");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  dir.appendFile("other.txt", " more");
  KJ_EXPECT(!resolvesWithin(watcher.onChange(), io, QUIET_TIMEOUT));
}

KJ_TEST("FileWatcher: a missing file in a populated directory fires on creation only") {
  // Other entries in the directory must neither fire nor fail the watch, whatever the backend
  // reports about them.
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "a");
  dir.writeFile("b.txt", "b");
  dir.writeFile("c.txt", "c");

  FileWatcher watcher;
  watcher.watch(dir.filePath("missing.txt"), kj::none);

  dir.appendFile("a.txt", " more");
  KJ_EXPECT(!resolvesWithin(watcher.onChange(), io, QUIET_TIMEOUT));

  auto change = watcher.onChange();
  dir.writeFile("missing.txt", "now it exists");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: watching a not-yet-existing file fires when it is created") {
  // Watching is by name through the parent directory, so the file itself need not exist yet.
  auto io = setupTokioAsyncIo();
  TempDir dir;

  FileWatcher watcher;
  watcher.watch(dir.filePath("missing.txt"), kj::none);

  auto change = watcher.onChange();
  dir.writeFile("missing.txt", "now it exists");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: canceling an armed onChange() and re-arming works") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  {
    auto armed = watcher.onChange();
    KJ_EXPECT(!armed.poll(io.getWaitScope()));
    // Dropped here while armed (fd registered with the tokio I/O driver).
  }

  auto change = watcher.onChange();
  dir.appendFile("a.txt", " two");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: multiple files in one watcher each fire on change") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "a");
  dir.writeFile("b.txt", "b");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);
  watcher.watch(dir.filePath("b.txt"), kj::none);

  {
    auto change = watcher.onChange();
    dir.appendFile("a.txt", "1");
    KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
  }
  // Drain any residual events from the first change before testing the second file.
  while (resolvesWithin(watcher.onChange(), io, QUIET_TIMEOUT)) {}
  {
    auto change = watcher.onChange();
    dir.appendFile("b.txt", "2");
    KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
  }
}

KJ_TEST("FileWatcher: two independent watchers do not cross-fire") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "a");
  dir.writeFile("b.txt", "b");

  FileWatcher w1;
  w1.watch(dir.filePath("a.txt"), kj::none);
  FileWatcher w2;
  w2.watch(dir.filePath("b.txt"), kj::none);

  // Changing a.txt fires w1 but must NOT fire w2 (each filters by basename).
  auto c2 = w2.onChange();
  {
    auto c1 = w1.onChange();
    dir.appendFile("a.txt", "1");
    KJ_EXPECT(resolvesWithin(kj::mv(c1), io, FIRE_TIMEOUT));
  }
  KJ_EXPECT(!resolvesWithin(kj::mv(c2), io, QUIET_TIMEOUT));
}

KJ_TEST("FileWatcher: teardown while a watch promise is armed") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "one");

  auto watcher = kj::heap<FileWatcher>();
  watcher->watch(dir.filePath("a.txt"), kj::none);

  auto armed = watcher->onChange();
  KJ_EXPECT(!armed.poll(io.getWaitScope()));

  // Either order is fine: the promise owns a share of the watcher state. Watcher first here.
  watcher = nullptr;
  armed = nullptr;
}

KJ_TEST("FileWatcher: attribute-only changes do not fire") {
  // Native watch masks exclude attribute changes (no IN_ATTRIB / NOTE_ATTRIB); a chmod must
  // not reload the config.
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  KJ_SYSCALL(chmod(dir.fileName("a.txt").cStr(), 0600));
  KJ_EXPECT(!resolvesWithin(watcher.onChange(), io, QUIET_TIMEOUT));
}

KJ_TEST("FileWatcher: a second concurrent onChange() is rejected, not raced") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  dir.writeFile("a.txt", "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);
  auto first = watcher.onChange();
  KJ_EXPECT(!first.poll(io.getWaitScope()));
  auto second = watcher.onChange();
  KJ_EXPECT_THROW_MESSAGE("at most one onChange()", second.wait(io.getWaitScope()));
}

KJ_TEST("FileWatcher: a symlinked file fires when its target elsewhere changes") {
  // workerd's config file and its own executable are symlinks under bazel run --watch. The
  // watcher stamps through the link and watches the target's directory too, so an edit to the
  // target -- in a directory nothing else watches -- still wakes it.
  auto io = setupTokioAsyncIo();
  TempDir linkDir;
  TempDir targetDir;
  targetDir.writeFile("real.txt", "one");
  KJ_SYSCALL(symlink(targetDir.fileName("real.txt").cStr(), linkDir.fileName("link.txt").cStr()));

  FileWatcher watcher;
  watcher.watch(linkDir.filePath("link.txt"), kj::none);

  auto change = watcher.onChange();
  targetDir.appendFile("real.txt", " two");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: a retargeted symlink fires, and its new target directory is watched too") {
  // The retarget itself is a change (the stamp follows the link to another inode). Afterwards
  // the file lives in a directory that was not watched when watch() ran; the watcher registers
  // it when it reports the retarget, so edits to the new target keep firing.
  auto io = setupTokioAsyncIo();
  TempDir linkDir;
  TempDir oldTarget;
  TempDir newTarget;
  oldTarget.writeFile("real.txt", "one");
  newTarget.writeFile("real.txt", "uno");
  KJ_SYSCALL(symlink(oldTarget.fileName("real.txt").cStr(), linkDir.fileName("link.txt").cStr()));

  FileWatcher watcher;
  watcher.watch(linkDir.filePath("link.txt"), kj::none);

  // Atomic retarget: a new link renamed over the old one, as `ln -sfn` does.
  auto change = watcher.onChange();
  KJ_SYSCALL(symlink(newTarget.fileName("real.txt").cStr(), linkDir.fileName("link.tmp").cStr()));
  linkDir.rename("link.tmp", "link.txt");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
  drain(watcher, io);

  auto afterRetarget = watcher.onChange();
  newTarget.appendFile("real.txt", " dos");
  KJ_EXPECT(resolvesWithin(kj::mv(afterRetarget), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: a symlink whose target does not exist yet fires when the target appears") {
  // --watch on a bundler's not-yet-written output through a link: the link's own directory sees
  // no event when the target is created elsewhere, so the target's directory must be watched
  // from the start (watcher.rs `canonical` follows the dangling link).
  auto io = setupTokioAsyncIo();
  TempDir linkDir;
  TempDir targetDir;
  auto target = targetDir.fileName("real.txt");
  KJ_SYSCALL(symlink(target.cStr(), linkDir.fileName("link.txt").cStr()));
  FileWatcher watcher;
  watcher.watch(linkDir.filePath("link.txt"));
  auto change = watcher.onChange();
  targetDir.writeFile("real.txt", "now it exists");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

#else  // _WIN32

KJ_TEST("FileWatcher: constructs on this platform (ReadDirectoryChangesW)") {
  auto io = setupTokioAsyncIo();
  FileWatcher watcher;
}

#endif

}  // namespace
}  // namespace kj_rs_io_test
