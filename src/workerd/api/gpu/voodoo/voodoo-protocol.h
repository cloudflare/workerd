// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This server interacts directly with the GPU, and listens on a UNIX socket for clients
// of the Dawn Wire protocol.

#pragma once

#include "voodoo-pipe.h"
#include <dawn/wire/WireServer.h>
#include <functional>
#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/exception.h>

// dawn buffer sizes
#define DAWNCMD_MSG_HEADER_SIZE 9 /* "D" <HEXBYTE>{8} */
#define DAWNCMD_MAX (4096 * 128)
#define DAWNCMD_BUFSIZE (DAWNCMD_MAX + DAWNCMD_MSG_HEADER_SIZE)

// protocol messages
//
// message        = dawncmdMsg
// dawncmdMsg     = "D" size
// size           = <uint32 in big-endian order>
//
#define MSGT_DAWNCMD 'D' /* Dawn command buffer */

namespace workerd::api::gpu::voodoo {

class DawnRemoteErrorHandler : public kj::TaskSet::ErrorHandler {
  kj::Own<kj::AsyncIoStream>& stream;

public:
  DawnRemoteErrorHandler(kj::Own<kj::AsyncIoStream>& s) : stream(s) {}
  void taskFailed(kj::Exception&& exception) override {
    KJ_LOG(ERROR, "task failed in dawn remote handler", exception);
    stream->shutdownWrite();
    stream->abortRead();
  }
};

struct DawnRemoteSerializer : public dawn::wire::CommandSerializer {
  DawnRemoteSerializer(kj::TaskSet& ts, kj::Own<kj::AsyncIoStream>& s) : taskset(ts), stream(s) {}
  kj::TaskSet& taskset;
  kj::Own<kj::AsyncIoStream>& stream;

  Pipe<DAWNCMD_BUFSIZE + 8> _rbuf; // incoming data (extra space for pipe impl)
  Pipe<4096> _wbuf;                // outgoing data (in addition to _dawnout)

  uint32_t _dawnCmdRLen = 0; // reamining nbytes to read as dawn command buffer

  // when we attempt to flush but we're still not done with the previous
  // flush operation we will signal for another flush to happen in the
  // future.
  bool needsFlush = false;

  // _dawnout is the dawn command buffer for outgoing Dawn command data
  struct {
    char bufs[2][DAWNCMD_BUFSIZE];
    char* writebuf = bufs[0];                    // buffer used for GetCmdSpace
    uint32_t writelen = DAWNCMD_MSG_HEADER_SIZE; // length of writebuf
    char* flushbuf = bufs[1];                    // buffer being written to stream
    uint32_t flushlen = 0;                       // length of flushbuf (>0 when flushing)
    uint32_t flushoffs = 0;                      // start offset of flushbuf
  } _dawnout;

  // _dawntmp is used for temporary storage of incoming dawn command buffers
  // in the case that they span across Pipe boundaries.
  char _dawntmp[DAWNCMD_MAX];

  // callbacks, client and server
  std::function<void(const char* data, size_t len)> onDawnBuffer;

  // main protocol method for handling incoming client commands
  kj::Promise<void> handleIncomingCommands();

  // internal methods
  bool maybeReadIncomingDawnCmd();
  bool readMsg();
  kj::Promise<void> actualFlush();

  // dawn::wire::CommandSerializer
  void* GetCmdSpace(size_t size) override;
  bool Flush() override;
  size_t GetMaximumAllocationSize() const override {
    KJ_LOG(INFO, "GetMaximumAllocationSize() was called");
    return DAWNCMD_MAX;
  };
};

} // namespace workerd::api::gpu::voodoo
