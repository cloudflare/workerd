#pragma once
// What the bridges of crates that serve kj streams natively (kj-hyper's ffi.rs) reference from
// kj-rs-io: the unwrap hook that recovers a kj-rs-io stream's native tokio socket, and the
// bridged operations on a *foreign* kj::AsyncIoStream that back a duplex pump. Kept out of
// bridge.h, which kj-rs-io's own bridge includes into every consumer of async-io.h and which
// therefore stays free of <kj/async-io.h> and the system headers it drags in.

#include <rust/cxx.h>

#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/refcount.h>

namespace kj_rs_io {

struct TokioStream;  // Opaque Rust type, defined in the generated ffi.rs.h.

// --- The unwrap fast path (implemented in async-io.c++).

// True if `stream` is the kj-rs-io wrapper accepted by unwrapTokioStream(). This lets Rust
// distinguish a foreign stream, which may use a fallback transport, from a native wrapper whose
// checked extraction failed and must be returned to the caller untouched.
bool isTokioStream(const kj::AsyncIoStream &stream);

// Recovers the native Rust stream out of a kj::AsyncIoStream created by kj-rs-io, leaving the
// wrapper hollow: any further I/O through the wrapper throws and getFd() returns none. Throws
// if `stream` is not a kj-rs-io stream, was already unwrapped, or has I/O promises in flight
// (the Rust side tracks in-flight operations, so this is detected, not a caller contract).
::rust::Box<TokioStream> unwrapTokioStream(kj::AsyncIoStream &stream);

// --- Bridged operations on a *foreign* kj::AsyncIoStream (one that did not originate in
// kj-rs-io and therefore cannot be unwrapped). These back the duplex-pump fallback of a native
// server's serve path (kj-hyper's serve.rs): the pump reads and writes the stream concurrently
// -- kj two-way streams support one read and one write in flight at once.
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

// Takes ownership of `stream` and returns its read direction; the write direction is derived
// from it with kjStreamWriteEnd().
inline kj::Own<KjStreamReadEnd> kjStreamReadEnd(kj::Own<kj::AsyncIoStream> stream) {
  return kj::heap<KjStreamReadEnd>(kj::rc<KjStreamShare>(kj::mv(stream)));
}

inline kj::Own<KjStreamWriteEnd> kjStreamWriteEnd(KjStreamReadEnd &read) {
  return kj::heap<KjStreamWriteEnd>(read.share.addRef());
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

}  // namespace kj_rs_io
