#include "kj-rs-io/file-watcher.h"

namespace kj_rs_io {

FileWatcher::FileWatcher(): inner(new_file_watcher()) {}

bool FileWatcher::isSupported() {
  return true;
}

void FileWatcher::watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile &>) {
  auto native = path.toNativeString(true);
  file_watcher_watch(*inner, ::rust::Str(native.begin(), native.size()));
}

kj::Promise<void> FileWatcher::onChange() {
  // Operation-start policy (async-io.h): the watch is armed inside the call, so a caller that
  // merely retains the promise still has its files watched.
  return file_watcher_on_change(*inner).eagerlyEvaluate(nullptr);
}

}  // namespace kj_rs_io
