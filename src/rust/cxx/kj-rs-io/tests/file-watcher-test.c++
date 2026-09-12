// Tests for kj_rs_io::FileWatcher (file-watcher.h), the tokio-loop replacement for workerd's
// --watch FileWatcher (Rust, over the notify crate: watcher.rs). Exercises the behaviors workerd
// depends on: plain modification, atomic replace-by-rename (editor saves), event
// queueing/coalescing across onChange() calls, the already-open-fd watch() signature,
// missing-file handling, and teardown/cancel while a watch promise is armed.

#include "kj-rs-io/async-io.h"
#include "kj-rs-io/file-watcher.h"

#include <kj/async.h>
#include <kj/debug.h>
#include <kj/filesystem.h>
#include <kj/test.h>

#include <cstdlib>
#include <cstring>

#if !_WIN32
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kj_rs_io_test {
namespace {

using kj_rs_io::FileWatcher;
using kj_rs_io::setupTokioAsyncIo;
using kj_rs_io::TokioAsyncIoContext;

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

struct TempDir {
  kj::String path;

  TempDir() {
    const char *base = getenv("TEST_TMPDIR");
    if (base == nullptr) base = "/tmp";
    auto tmpl = kj::str(base, "/kj-rs-io-file-watcher-test.XXXXXX");
    KJ_ASSERT(mkdtemp(tmpl.begin()) != nullptr, strerror(errno));
    path = kj::mv(tmpl);
  }

  ~TempDir() noexcept(false) {
    // Best-effort cleanup; TEST_TMPDIR is wiped by bazel anyway.
    auto cmd = kj::str("rm -rf ", path);
    (void)system(cmd.cStr());
  }

  kj::String fileName(kj::StringPtr name) {
    return kj::str(path, "/", name);
  }

  kj::Path filePath(kj::StringPtr name) {
    auto full = fileName(name);
    KJ_ASSERT(full.startsWith("/"));
    return kj::Path::parse(full.slice(1));
  }
};

void writeFile(kj::StringPtr path, kj::StringPtr content) {
  kj::OwnFd fd = KJ_SYSCALL_FD(open(path.cStr(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
  KJ_SYSCALL(write(fd, content.begin(), content.size()));
}

void appendFile(kj::StringPtr path, kj::StringPtr content) {
  kj::OwnFd fd = KJ_SYSCALL_FD(open(path.cStr(), O_WRONLY | O_APPEND));
  KJ_SYSCALL(write(fd, content.begin(), content.size()));
}

// After a change fired, drains any further already-queued events so the next onChange() call
// starts from a quiet state (mirrors what workerd's waitForChanges() settle loop achieves).
void drain(FileWatcher &watcher, TokioAsyncIoContext &io) {
  while (resolvesWithin(watcher.onChange(), io, QUIET_TIMEOUT)) {}
}

// =======================================================================================
// Tests

KJ_TEST("FileWatcher: supported on this platform") {
  auto io = setupTokioAsyncIo();
  FileWatcher watcher;
  KJ_EXPECT(watcher.isSupported());
}

KJ_TEST("FileWatcher: modification fires onChange") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  auto change = watcher.onChange();
  appendFile(dir.fileName("a.txt"), " two");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: change before onChange() is called is not lost") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  // Modify before anyone is waiting: the event queues in the kernel.
  appendFile(dir.fileName("a.txt"), " two");
  KJ_EXPECT(resolvesWithin(watcher.onChange(), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: atomic replace-by-rename fires onChange") {
  // Editors typically save by writing a temporary file and rename(2)ing it over the target.
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  auto change = watcher.onChange();
  writeFile(dir.fileName("a.txt.tmp"), "two");
  KJ_SYSCALL(rename(dir.fileName("a.txt.tmp").cStr(), dir.fileName("a.txt").cStr()));
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: watching via an already-open file handle") {
  // workerd passes the config files' already-open kj::ReadableFile to watch(); watching is by
  // name, so the handle is accepted and ignored, and may be closed right away.
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "one");

  auto file = kj::newDiskReadableFile(KJ_SYSCALL_FD(open(dir.fileName("a.txt").cStr(), O_RDONLY)));

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), *file);
  file = nullptr;  // The original handle may be closed; the watch must survive.

  auto change = watcher.onChange();
  appendFile(dir.fileName("a.txt"), " two");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: rapid changes coalesce; watcher stays armed for later "
        "changes") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  // A burst of changes produces one resolution per onChange() call (not one per event), ...
  auto change = watcher.onChange();
  appendFile(dir.fileName("a.txt"), " two");
  appendFile(dir.fileName("a.txt"), " three");
  appendFile(dir.fileName("a.txt"), " four");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));

  // ... and once the queue is drained, the watcher is quiet ...
  drain(watcher, io);

  // ... but still armed: a fresh change fires a fresh onChange().
  auto later = watcher.onChange();
  appendFile(dir.fileName("a.txt"), " five");
  KJ_EXPECT(resolvesWithin(kj::mv(later), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: deleting the watched file fires; a recreated file is tracked where the "
        "backend can") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  // Delete: both backends report it (inotify IN_DELETE on the directory; kqueue NOTE_DELETE on
  // the open fd).
  auto change = watcher.onChange();
  KJ_SYSCALL(unlink(dir.fileName("a.txt").cStr()));
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
  drain(watcher, io);

  // Recreate and modify: watched by name, the new file is picked up on every backend (the
  // native kqueue watcher watched the deleted inode's fd and could not; this one re-arms the
  // file watch on create).
  auto later = watcher.onChange();
  writeFile(dir.fileName("a.txt"), "two");
  appendFile(dir.fileName("a.txt"), " three");
  KJ_EXPECT(resolvesWithin(kj::mv(later), io, FIRE_TIMEOUT));
  drain(watcher, io);
  // ...and subsequent writes to the recreated file keep firing.
  auto again = watcher.onChange();
  appendFile(dir.fileName("a.txt"), " four");
  KJ_EXPECT(resolvesWithin(kj::mv(again), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: the onChange() promise outlives the FileWatcher object") {
  // The promise co-owns the watcher state, so destroying the FileWatcher first must neither
  // crash nor invalidate the pending promise: a change made afterwards still resolves it.
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "one");

  kj::Promise<void> change = nullptr;
  {
    FileWatcher watcher;
    watcher.watch(dir.filePath("a.txt"), kj::none);
    change = watcher.onChange();
    KJ_EXPECT(!change.poll(io.getWaitScope()));
  }
  appendFile(dir.fileName("a.txt"), " two");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: unrelated files in the same directory do not fire (basename filtering)") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "one");
  writeFile(dir.fileName("other.txt"), "other");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  appendFile(dir.fileName("other.txt"), " more");
  KJ_EXPECT(!resolvesWithin(watcher.onChange(), io, QUIET_TIMEOUT));
}

KJ_TEST("FileWatcher: watching a not-yet-existing file fires when it is created") {
  // Watching is by name through the parent directory, so the file itself need not exist yet.
  auto io = setupTokioAsyncIo();
  TempDir dir;

  FileWatcher watcher;
  watcher.watch(dir.filePath("missing.txt"), kj::none);

  auto change = watcher.onChange();
  writeFile(dir.fileName("missing.txt"), "now it exists");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: canceling an armed onChange() and re-arming works") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  {
    auto armed = watcher.onChange();
    KJ_EXPECT(!armed.poll(io.getWaitScope()));
    // Dropped here while armed (fd registered with the tokio I/O driver).
  }

  auto change = watcher.onChange();
  appendFile(dir.fileName("a.txt"), " two");
  KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
}

KJ_TEST("FileWatcher: multiple files in one watcher each fire on change") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "a");
  writeFile(dir.fileName("b.txt"), "b");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);
  watcher.watch(dir.filePath("b.txt"), kj::none);

  {
    auto change = watcher.onChange();
    appendFile(dir.fileName("a.txt"), "1");
    KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
  }
  // Drain any residual events from the first change before testing the second file.
  while (resolvesWithin(watcher.onChange(), io, QUIET_TIMEOUT)) {}
  {
    auto change = watcher.onChange();
    appendFile(dir.fileName("b.txt"), "2");
    KJ_EXPECT(resolvesWithin(kj::mv(change), io, FIRE_TIMEOUT));
  }
}

KJ_TEST("FileWatcher: two independent watchers do not cross-fire") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "a");
  writeFile(dir.fileName("b.txt"), "b");

  FileWatcher w1;
  w1.watch(dir.filePath("a.txt"), kj::none);
  FileWatcher w2;
  w2.watch(dir.filePath("b.txt"), kj::none);

  // Changing a.txt fires w1 but must NOT fire w2 (each filters by basename).
  auto c2 = w2.onChange();
  {
    auto c1 = w1.onChange();
    appendFile(dir.fileName("a.txt"), "1");
    KJ_EXPECT(resolvesWithin(kj::mv(c1), io, FIRE_TIMEOUT));
  }
  KJ_EXPECT(!resolvesWithin(kj::mv(c2), io, QUIET_TIMEOUT));
}

KJ_TEST("FileWatcher: teardown while a watch promise is armed") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "one");

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
  writeFile(dir.fileName("a.txt"), "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);

  KJ_SYSCALL(chmod(dir.fileName("a.txt").cStr(), 0600));
  KJ_EXPECT(!resolvesWithin(watcher.onChange(), io, QUIET_TIMEOUT));
}

KJ_TEST("FileWatcher: a second concurrent onChange() is rejected, not raced") {
  auto io = setupTokioAsyncIo();
  TempDir dir;
  writeFile(dir.fileName("a.txt"), "one");

  FileWatcher watcher;
  watcher.watch(dir.filePath("a.txt"), kj::none);
  auto first = watcher.onChange();
  KJ_EXPECT(!first.poll(io.getWaitScope()));
  auto second = watcher.onChange();
  KJ_EXPECT_THROW_MESSAGE("at most one onChange()", second.wait(io.getWaitScope()));
}

#else  // _WIN32

KJ_TEST("FileWatcher: supported on this platform (ReadDirectoryChangesW)") {
  auto io = setupTokioAsyncIo();
  FileWatcher watcher;
  KJ_EXPECT(watcher.isSupported());
}

#endif

}  // namespace
}  // namespace kj_rs_io_test
