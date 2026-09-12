#pragma once
// The C++ the kj-rs-io cxx bridge (ffi.rs) references: the accessors for a multi-piece write.
// Everything else crosses as shared structs the generated header defines (SocketAddress,
// PeerStream, ...); the KJ interfaces live in async-io.h.
//
// Deliberately free of system headers: this header reaches every consumer of async-io.h through
// the generated ffi.rs.h, and <winsock2.h> would bring <windows.h> -- and its DELETE / ERROR
// macros -- along, breaking kj/compat/http.h in the same translation unit.

#include <rust/cxx.h>

#include <kj/common.h>
#include <kj/debug.h>

namespace kj_rs_io {

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

// The `index`th piece. Bounds-checked: an out-of-range index throws (an Err on the Rust side)
// rather than reading past the array.
inline ::rust::Slice<const uint8_t> kjPiece(const KjPieces &pieces, size_t index) {
  KJ_REQUIRE(index < pieces.pieces.size(), "piece index out of range", index, pieces.pieces.size());
  auto piece = pieces.pieces[index];
  return ::rust::Slice<const uint8_t>(piece.begin(), piece.size());
}

}  // namespace kj_rs_io
