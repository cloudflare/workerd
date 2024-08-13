// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include "voodoo-protocol.h"

namespace workerd::api::gpu::voodoo {

static void decodeDawnCmdHeader(const char* src, uint32_t* dawncmdlen) {
  KJ_ASSERT(src[0] == MSGT_DAWNCMD);
  *dawncmdlen = ntohl(*((uint32_t*)&src[1]));
}

// encodeDawnCmdHeader writes a MSGT_DAWNCMD header of DAWNCMD_MSG_HEADER_SIZE bytes to dst.
static void encodeDawnCmdHeader(char* dst, uint32_t dawncmdlen) {
  dst[0] = MSGT_DAWNCMD;
  *((uint32_t*)&dst[1]) = htonl(dawncmdlen);
}

kj::Promise<void> DawnRemoteSerializer::handleIncomingCommands() {
  for (;;) {
    ssize_t n = co_await _rbuf.readFromStream(stream, _rbuf.cap());
    if (n == 0) {
      KJ_LOG(INFO, "EOF received while reading from stream");
      co_return;
    }

    KJ_LOG(INFO, "read bytes from stream", n, _rbuf.len());
    if (_dawnCmdRLen > 0) {
      maybeReadIncomingDawnCmd();
    } else {
      if (!readMsg()) {
        co_return;
      }
    }
  }
}

kj::Promise<void> DawnRemoteSerializer::actualFlush() {
  // if we are flushing Dawn command data, do that before draining _wbuf
  if (_dawnout.flushlen != 0) {
    KJ_ASSERT(_dawnout.flushlen > _dawnout.flushoffs);
    uint32_t len = _dawnout.flushlen - _dawnout.flushoffs;
    KJ_LOG(INFO, "_dawnout flush", _dawnout.flushoffs, len);
    const kj::byte* ptr = reinterpret_cast<const kj::byte*>(&_dawnout.flushbuf[_dawnout.flushoffs]);
    co_await stream->write(kj::arrayPtr<const kj::byte>(ptr, len));

    _dawnout.flushoffs += len;
    _dawnout.flushlen = 0;
  }

  // drain _wbuf
  size_t nbyte = _wbuf.len();
  if (nbyte > 0) {
    co_await _wbuf.writeToStream(stream, nbyte);
  }

  if (needsFlush) {
    needsFlush = false;
    Flush();
  }
}

// readMsg reads a protocol message from the read buffer (_rbuf)
bool DawnRemoteSerializer::readMsg() {
  char tmp[DAWNCMD_MSG_HEADER_SIZE + 1];
  while (_rbuf.len() > 0) {
    switch (_rbuf.at(0)) {

    case MSGT_DAWNCMD:
      KJ_LOG(INFO, "MSGT_DAWNCMD", _rbuf.len(), _rbuf.at(0));
      if (_rbuf.len() >= DAWNCMD_MSG_HEADER_SIZE) {
        _rbuf.read(tmp, DAWNCMD_MSG_HEADER_SIZE);
        decodeDawnCmdHeader(tmp, &_dawnCmdRLen);

        KJ_LOG(INFO, "will start reading dawn command buffer", _dawnCmdRLen);
        if (!maybeReadIncomingDawnCmd()) {
          // read was succesful but dawn command is still incomplete
          return true;
        };
      }
      break;

    default:
      // unexpected/corrupt message data
      char c = _rbuf.at(0);
      KJ_LOG(ERROR, "unexpected message received", c, _rbuf.len());
      return false;
    }
  }

  return true;
}

bool DawnRemoteSerializer::maybeReadIncomingDawnCmd() {
  KJ_REQUIRE(_dawnCmdRLen > 0, "no data to read as dawn command");
  KJ_REQUIRE(_dawnCmdRLen <= DAWNCMD_MAX, "length of data to read as dawn command is too high");

  if (_rbuf.len() < _dawnCmdRLen) {
    KJ_LOG(INFO, "dawn command is still incomplete", _rbuf.len(), _dawnCmdRLen);
    return false;
  }

  // onDawnBuffer expects a contiguous memory segment; attempt to simply reference
  // the data in rbuf. takeRef returns null if the data is not available as a contiguous
  // segement, in which case we resort to copying it into a temporary buffer.
  const char* buf = _rbuf.takeRef(_dawnCmdRLen);
  if (buf == nullptr) {
    // copy into temporary buffer
    KJ_LOG(INFO, "data is not contiguous, will copy to temporary buffer _dawntmp");
    _rbuf.read(_dawntmp, _dawnCmdRLen);
    buf = _dawntmp;
  }
  onDawnBuffer(buf, _dawnCmdRLen);
  _dawnCmdRLen = 0;
  return true;
}

void* DawnRemoteSerializer::GetCmdSpace(size_t size) {
  KJ_LOG(INFO, "GetCmdSpace() was called", size);
  KJ_ASSERT(size <= DAWNCMD_MAX);
  if (DAWNCMD_BUFSIZE - _dawnout.writelen < size) {
    KJ_LOG(ERROR,
           "GetCmdSpace() could not allocate enough space for the dawn command and message header",
           size, _dawnout.writelen);
    return nullptr;
  }
  char* result = &_dawnout.writebuf[_dawnout.writelen];
  _dawnout.writelen += size;
  return result;
}

bool DawnRemoteSerializer::Flush() {
  KJ_LOG(INFO, "Flush() was called", _dawnout.writelen, _dawnout.flushlen);

  if (_dawnout.flushlen != 0) {
    /* not done flushing previous buffer */
    needsFlush = true;
    return false;
  }

  if (_dawnout.writelen > DAWNCMD_MSG_HEADER_SIZE) {
    // write header (preallocated at writebuf[0..DAWNCMD_MSG_HEADER_SIZE])
    encodeDawnCmdHeader(_dawnout.writebuf, _dawnout.writelen - DAWNCMD_MSG_HEADER_SIZE);

    // swap buffers
    char* buf1 = _dawnout.flushbuf;
    _dawnout.flushbuf = _dawnout.writebuf;
    _dawnout.writebuf = buf1;

    // setup flush state
    _dawnout.flushlen = _dawnout.writelen;
    _dawnout.flushoffs = 0;
    taskset.add(actualFlush());

    // reset write
    _dawnout.writelen = DAWNCMD_MSG_HEADER_SIZE;
  } else {
    KJ_ASSERT(_dawnout.writelen == DAWNCMD_MSG_HEADER_SIZE);
  }
  return true;
};

} // namespace workerd::api::gpu::voodoo
