#pragma once
// Declarations needed by the kj-rs-io cxx bridge (lib.rs) itself. The full C++ API lives in
// kj-rs-io/async-io.h; this header only exposes what the generated bridge code references:
// kj::AsyncIoStream (as an opaque extern C++ type), the unwrap hook, and the bridged stream
// operations backing serve_kj_stream()'s pump fallback (serve.rs).

#include <rust/cxx.h>

#include <kj/async-io.h>

namespace kj_rs_io {

struct TokioStream;  // Opaque Rust type, defined in the generated lib.rs.h.

// Recovers the native Rust stream out of a kj::AsyncIoStream created by kj-rs-io (the "unwrap
// fast path"), leaving the wrapper hollow: any further I/O through the wrapper throws. Throws if
// `stream` is not a kj-rs-io stream, was already unwrapped, or has I/O promises in flight (the
// Rust side tracks in-flight operations, so this is detected, not a caller contract).
//
// Implemented in async-io.c++. Rust code calls this through kj_rs_io::unwrap_kj_stream().
::rust::Box<TokioStream> unwrapTokioStream(kj::AsyncIoStream &stream);

// --- Bridged operations on a *foreign* kj::AsyncIoStream (one that did not originate in
// kj-rs-io and therefore cannot be unwrapped). These back the duplex-pump fallback of
// serve_kj_stream() (serve.rs): the pump owns the stream and reads and writes it through
// concurrent Rust *shared* borrows — kj two-way streams support one read and one write in
// flight at once.

// A Rust shared borrow (&KjAsyncIoStream) arrives in C++ as a const&: the constness is cxx's
// wire format for "shared", not a property of the object — the stream is uniquely owned by the
// pump and never actually const. NOTE the meaning mismatch with KJ convention: KJ const means
// *thread-safe* (Rust's Sync), but this shared-ness is the weaker property — reentrant use from
// one thread's async tasks (the bridge types are !Send/!Sync, so Rust can never move these
// borrows off the KJ event-loop thread that owns the stream). Recover the callable reference
// here, once.
inline kj::AsyncIoStream &pumpStream(const kj::AsyncIoStream &stream) {
  return const_cast<kj::AsyncIoStream &>(stream);
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

// Corresponds to kj::AsyncIoStream::tryRead(buffer, minBytes, buffer.size()).
inline kj::Promise<size_t> kjStreamTryRead(
    const kj::AsyncIoStream &stream, ::rust::Slice<uint8_t> buffer, size_t minBytes) {
  return pumpStream(stream).tryRead(buffer.data(), minBytes, buffer.size());
}

// Corresponds to kj::AsyncIoStream::write(buffer) (write-all semantics).
inline kj::Promise<void> kjStreamWrite(
    const kj::AsyncIoStream &stream, ::rust::Slice<const uint8_t> buffer) {
  return pumpStream(stream).write(kj::arrayPtr(buffer.data(), buffer.size()));
}

// Corresponds to kj::AsyncIoStream::shutdownWrite().
inline void kjStreamShutdownWrite(const kj::AsyncIoStream &stream) {
  pumpStream(stream).shutdownWrite();
}

// The stream's underlying raw OS socket handle -- a Unix fd (kj::AsyncIoStream::getFd()) or a
// win32 SOCKET (kj::AsyncIoStream::getWin32Handle()) -- widened to int64, or -1 if it exposes
// none. int64 fits both losslessly with one sentinel: a Unix fd is a non-negative int, and
// INVALID_SOCKET (~0 as UINT_PTR) is exactly -1 as int64 (live win64 SOCKET values fit in 32
// bits per the Windows handle-interoperability guarantee, so they never collide with -1).
// Backs the handle tier of take_kj_socket() (ffi.rs); see that function's docs for the
// caller-asserted "the handle carries the stream's own bytes" contract (wrappers such as
// kj::TlsConnection forward getFd() to their *transport* socket, which this tier must never be
// used on).
inline int64_t kjStreamGetHandle(const kj::AsyncIoStream &stream) {
#if _WIN32
  // Validated by Windows CI; mirrors the unix arm.
  KJ_IF_SOME(handle, stream.getWin32Handle()) {
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(handle));
  }
  return -1;
#else
  return stream.getFd().orDefault(-1);
#endif
}

}  // namespace kj_rs_io
