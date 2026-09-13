#pragma once
// Declarations needed by the kj-rs-io cxx bridge (ffi.rs) itself. The full C++ API lives in
// kj-rs-io/async-io.h; this header only exposes what the generated bridge code references:
// kj::AsyncIoStream (as an opaque extern C++ type), the unwrap hook, the bridged stream
// operations backing serve_kj_stream()'s pump fallback (serve.rs), and the peer-filter shims
// through which Rust applies restrictPeers() policy.

#include "kj-rs-io/peer-filter.h"

#include <rust/cxx.h>

#include <kj/async-io.h>
#include <kj/debug.h>

#include <cstring>

#if _WIN32
#include <winsock2.h>
// windows.h (pulled in by winsock2.h) defines ERROR as a macro, which breaks KJ_LOG(ERROR).
#include <kj/windows-sanity.h>
#else
#include <sys/socket.h>
#endif

namespace kj_rs_io {

struct TokioStream;  // Opaque Rust type, defined in the generated ffi.rs.h.

// True if `stream` is the kj-rs-io wrapper accepted by unwrapTokioStream(). This lets Rust
// distinguish a foreign stream, which may use a fallback transport, from a native wrapper whose
// checked extraction failed and must be returned to the caller untouched.
bool isTokioStream(const kj::AsyncIoStream &stream);

// Recovers the native Rust stream out of a kj::AsyncIoStream created by kj-rs-io (the "unwrap
// fast path"), leaving the wrapper hollow: any further I/O through the wrapper throws. Throws if
// `stream` is not a kj-rs-io stream, was already unwrapped, or has I/O promises in flight (the
// Rust side tracks in-flight operations, so this is detected, not a caller contract).
//
// Implemented in async-io.c++. Rust code calls this through kj_rs_io::unwrap_kj_stream().
::rust::Box<TokioStream> unwrapTokioStream(kj::AsyncIoStream &stream);

// --- restrictPeers() policy, applied from Rust.
//
// The policy object stays C++ (PeerFilter wraps KJ's own kj::_::NetworkFilter, see
// peer-filter.h); the connect / accept / parse loops that consult it live in Rust and reach it
// through these two shims. Rust owns a kj::Own<NetworkFilter> share for the duration of each
// operation (a refcount share for kj-rs-io's own filters; for a caller-provided
// LowLevelAsyncIoProvider::NetworkFilter& -- wrapListenSocketFd -- a non-owning Own, KJ's own
// "the filter outlives the receiver" contract).

using NetworkFilter = kj::LowLevelAsyncIoProvider::NetworkFilter;

// Copies raw sockaddr bytes from the bridge (rust::Vec<u8>/Slice data is 1-aligned) into an
// aligned, zero-filled sockaddr_storage for kj's NetworkFilter, which reads them as a struct.
struct AlignedSockaddr {
  struct sockaddr_storage storage;
  kj::uint length;

  explicit AlignedSockaddr(::rust::Slice<const uint8_t> raw): length(raw.size()) {
    memset(&storage, 0, sizeof(storage));
    KJ_REQUIRE(raw.size() <= sizeof(storage), "sockaddr too large");
    memcpy(&storage, raw.data(), raw.size());
  }

  struct sockaddr *get() {
    return reinterpret_cast<struct sockaddr *>(&storage);
  }
};

// kj::LowLevelAsyncIoProvider::NetworkFilter::shouldAllow over raw sockaddr bytes: the
// connect-time and accept-time check.
inline bool networkFilterShouldAllow(NetworkFilter &filter, ::rust::Slice<const uint8_t> addr) {
  AlignedSockaddr aligned(addr);
  return filter.shouldAllow(aligned.get(), aligned.length);
}

// kj::_::NetworkFilter::shouldAllowParse over raw sockaddr bytes: KJ's parse-time check of a
// literal address's family (see peer-filter.h).
inline bool peerFilterShouldAllowParse(
    const PeerFilter &filter, ::rust::Slice<const uint8_t> addr) {
  AlignedSockaddr aligned(addr);
  return filter.shouldAllowParse(aligned.get(), aligned.length);
}

// --- Bridged operations on a *foreign* kj::AsyncIoStream (one that did not originate in
// kj-rs-io and therefore cannot be unwrapped). These back the duplex-pump fallback of
// serve_kj_stream() (serve.rs): the pump owns the stream and reads and writes it concurrently --
// kj two-way streams support one read and one write in flight at once.
//
// Each direction is its own small object referring to the stream, so Rust holds a genuinely
// exclusive `Pin<&mut KjStreamReadEnd>` / `Pin<&mut KjStreamWriteEnd>` per direction (one
// in-flight operation each, enforced by the borrow checker) without ever needing two `&mut`s to
// the one stream or a const_cast. Both ends refer to the stream and must not outlive it; the Rust
// side (ffi.rs `split_kj_stream`) ties their lifetime to a `&mut` borrow of the stream's owner.

struct KjStreamReadEnd {
  explicit KjStreamReadEnd(kj::AsyncIoStream &stream): stream(stream) {}
  KJ_DISALLOW_COPY_AND_MOVE(KjStreamReadEnd);
  kj::AsyncIoStream &stream;
};

struct KjStreamWriteEnd {
  explicit KjStreamWriteEnd(kj::AsyncIoStream &stream): stream(stream) {}
  KJ_DISALLOW_COPY_AND_MOVE(KjStreamWriteEnd);
  kj::AsyncIoStream &stream;
};

inline kj::Own<KjStreamReadEnd> kjStreamReadEnd(kj::AsyncIoStream &stream) {
  return kj::heap<KjStreamReadEnd>(stream);
}

inline kj::Own<KjStreamWriteEnd> kjStreamWriteEnd(kj::AsyncIoStream &stream) {
  return kj::heap<KjStreamWriteEnd>(stream);
}

// Corresponds to kj::AsyncIoStream::tryRead(buffer, minBytes, buffer.size()). The buffer is the
// Rust pump's own initialized Vec, hence a plain Slice.
inline kj::Promise<size_t> kjReadEndTryRead(
    KjStreamReadEnd &end, ::rust::Slice<uint8_t> buffer, size_t minBytes) {
  return end.stream.tryRead(buffer.data(), minBytes, buffer.size());
}

// Corresponds to kj::AsyncIoStream::write(buffer) (write-all semantics).
inline kj::Promise<void> kjWriteEndWrite(
    KjStreamWriteEnd &end, ::rust::Slice<const uint8_t> buffer) {
  return end.stream.write(kj::arrayPtr(buffer.data(), buffer.size()));
}

// Corresponds to kj::AsyncIoStream::shutdownWrite().
inline void kjWriteEndShutdownWrite(KjStreamWriteEnd &end) {
  end.stream.shutdownWrite();
}

// The pieces of a kj::AsyncOutputStream::write(pieces) call, handed to Rust for a vectored write
// (stream_write_pieces, ffi.rs). Opaque to cxx; Rust reads it through the two accessors below.
// Owned by the C++ coroutine frame that awaits the bridged future (TokioAsyncIoStream::
// writePieces); the piece buffers are the caller's, valid until that promise settles.
struct KjPieces {
  kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces;
};

inline size_t kjPiecesCount(const KjPieces &pieces) {
  return pieces.pieces.size();
}

inline ::rust::Slice<const uint8_t> kjPiece(const KjPieces &pieces, size_t index) {
  auto piece = pieces.pieces[index];
  return ::rust::Slice<const uint8_t>(piece.begin(), piece.size());
}

}  // namespace kj_rs_io
