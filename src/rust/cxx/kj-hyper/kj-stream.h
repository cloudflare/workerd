#pragma once
// The C++ side of serving a kj::AsyncIoStream natively (serve.rs): taking a kj-rs-io stream's
// tokio socket (or a RustStream's Rust stream) out of its wrapper, and the bridged operations
// on a *foreign* kj::AsyncIoStream that back the pump fallback. Included by kj-hyper's cxx
// bridge (ffi.rs).
#include "kj-rs-io/async-io.h"

#include <rust/cxx.h>

#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/refcount.h>

// The native path recognizes kj-rs-io streams by dynamic_cast. Without RTTI every stream would
// silently be treated as foreign and take the pump path; make that a build error rather than a
// performance mystery.
#if KJ_NO_RTTI
#error "kj-hyper's native-serve path requires RTTI (dynamicDowncastIfAvailable); KJ_NO_RTTI is set"
#endif

namespace workerd::rust::kj_hyper {

// --- The unwrap fast path.

// True if `stream` is a kj-rs-io wrapper: the only kind whose socket can be taken natively.
inline bool isTokioStream(const kj::AsyncIoStream &stream) {
  return kj::dynamicDowncastIfAvailable<const kj_rs_io::TokioAsyncIoStream>(stream) != kj::none;
}

// Takes the native stream out of a kj-rs-io wrapper and destroys the wrapper. The caller must
// have checked isTokioStream(); whether the socket can actually be taken (no I/O in flight) is
// decided on the Rust side (TokioStream::into_socket), which hands the stream back for
// wrapTokioStream() if not.
inline ::rust::Box<kj_rs_io::TokioStream> releaseTokioStream(kj::Own<kj::AsyncIoStream> stream) {
  auto &tokioStream =
      KJ_REQUIRE_NONNULL(kj::dynamicDowncastIfAvailable<kj_rs_io::TokioAsyncIoStream>(*stream),
          "stream is not a kj-rs-io tokio-backed stream; cannot take its socket");
  return tokioStream.release();
}

// The inverse: a fresh kj-rs-io wrapper over a native stream that could not be taken.
inline kj::Own<kj::AsyncIoStream> wrapTokioStream(::rust::Box<kj_rs_io::TokioStream> stream) {
  return kj::heap<kj_rs_io::TokioAsyncIoStream>(kj::mv(stream));
}

// --- Streams that are Rust underneath: a RustStream (rust_stream.rs) wrapped as a
// kj::AsyncIoStream by RustAsyncIoStream (hyper-server-ffi.c++, which defines these three; this
// header is included by the generated bridge and cannot see the generated RustStream type).
struct RustStream;

// True if `stream` is a RustAsyncIoStream wrapper whose Rust stream may be taken apart now: no kj
// operation still holds it, on the owning thread (RustStream::can_release).
bool isReleasableRustStream(const kj::AsyncIoStream &stream);

// Takes the Rust stream out of the wrapper -- cancelling the wrapper's pump driver first -- and
// destroys the wrapper. The caller must have checked isReleasableRustStream().
::rust::Box<RustStream> releaseRustStream(kj::Own<kj::AsyncIoStream> stream);

// The inverse: a kj::AsyncIoStream over a Rust stream, for kj consumers.
kj::Own<kj::AsyncIoStream> wrapRustStream(::rust::Box<RustStream> stream);

// --- Bridged operations on a *foreign* kj::AsyncIoStream (one that did not originate in
// kj-rs-io and therefore has no socket to take). These back the duplex-pump fallback of
// serve_kj_stream() (serve.rs): the pump reads and writes the stream concurrently -- kj two-way
// streams support one read and one write in flight at once.
//
// Ownership: the stream lives in a refcounted holder, and each direction is its own small
// object owning a share of it. Rust holds the two ends by kj::Own and drives each through an
// exclusive `Pin<&mut _>` (one in-flight operation per direction, enforced by the borrow
// checker) without ever needing two `&mut`s to the one stream or a const_cast; the stream is
// destroyed when the last end is, so no end can outlive it -- C++ ownership enforces that, not
// a lifetime convention on the Rust side.
struct KjStreamShare final: public kj::Refcounted {
  explicit KjStreamShare(kj::Own<kj::AsyncIoStream> stream): stream(kj::mv(stream)) {}
  kj::Own<kj::AsyncIoStream> stream;
};

struct KjStreamReadEnd {
  explicit KjStreamReadEnd(kj::Rc<KjStreamShare> share): share(kj::mv(share)) {}
  KJ_DISALLOW_COPY_AND_MOVE(KjStreamReadEnd);
  kj::Rc<KjStreamShare> share;
};

struct KjStreamWriteEnd {
  explicit KjStreamWriteEnd(kj::Rc<KjStreamShare> share): share(kj::mv(share)) {}
  KJ_DISALLOW_COPY_AND_MOVE(KjStreamWriteEnd);
  kj::Rc<KjStreamShare> share;
};
// whenWriteDisconnected(), which kj allows alongside the in-flight read and write.
struct KjStreamWatchEnd {
  explicit KjStreamWatchEnd(kj::Rc<KjStreamShare> share): share(kj::mv(share)) {}
  KJ_DISALLOW_COPY_AND_MOVE(KjStreamWatchEnd);
  kj::Rc<KjStreamShare> share;
};

// Takes ownership of `stream` and returns its read direction; the write direction is derived
// from it with kjStreamWriteEnd().
inline kj::Own<KjStreamReadEnd> kjStreamReadEnd(kj::Own<kj::AsyncIoStream> stream) {
  return kj::heap<KjStreamReadEnd>(kj::rc<KjStreamShare>(kj::mv(stream)));
}

inline kj::Own<KjStreamWriteEnd> kjStreamWriteEnd(KjStreamReadEnd &read) {
  return kj::heap<KjStreamWriteEnd>(read.share.addRef());
}
inline kj::Own<KjStreamWatchEnd> kjStreamWatchEnd(KjStreamReadEnd &read) {
  return kj::heap<KjStreamWatchEnd>(read.share.addRef());
}

// Corresponds to kj::AsyncIoStream::tryRead(buffer, minBytes, buffer.size()). The buffer is the
// Rust pump's own initialized Vec, hence a plain Slice.
inline kj::Promise<size_t> kjReadEndTryRead(
    KjStreamReadEnd &end, ::rust::Slice<uint8_t> buffer, size_t minBytes) {
  return end.share->stream->tryRead(buffer.data(), minBytes, buffer.size());
}

// Corresponds to kj::AsyncIoStream::write(buffer) (write-all semantics).
inline kj::Promise<void> kjWriteEndWrite(
    KjStreamWriteEnd &end, ::rust::Slice<const uint8_t> buffer) {
  return end.share->stream->write(kj::arrayPtr(buffer.data(), buffer.size()));
}

// Corresponds to kj::AsyncIoStream::shutdownWrite().
inline void kjWriteEndShutdownWrite(KjStreamWriteEnd &end) {
  end.share->stream->shutdownWrite();
}
// Corresponds to kj::AsyncOutputStream::whenWriteDisconnected().
inline kj::Promise<void> kjWatchEndWhenWriteDisconnected(KjStreamWatchEnd &end) {
  return end.share->stream->whenWriteDisconnected();
}

}  // namespace workerd::rust::kj_hyper
