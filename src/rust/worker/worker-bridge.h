// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/worker-interface.h>
#include <workerd/io/outcome.capnp.h>
#include <kj/async.h>
#include <kj/time.h>
#include <kj/string.h>
#include <kj/compat/http.h>
#include <capnp/compat/http-over-capnp.h>

namespace workerd::rust::worker {

// Type aliases for KJ types
using HttpMethod = kj::HttpMethod;
using HttpHeaders = kj::HttpHeaders;
using AsyncInputStream = kj::AsyncInputStream;
using AsyncIoStream = kj::AsyncIoStream;
using HttpServiceResponse = kj::HttpService::Response;
using ConnectResponse = kj::HttpService::ConnectResponse;
using HttpConnectSettings = kj::HttpConnectSettings;
using Date = kj::Date;
using StringPtr = kj::StringPtr;
using TaskSet = kj::TaskSet;

// Type aliases for workerd types
using IoContext_IncomingRequest = workerd::IoContext_IncomingRequest;
using Frankenvalue = workerd::Frankenvalue;

// Type aliases for capnp types
using HttpOverCapnpFactory = capnp::HttpOverCapnpFactory;
using ByteStreamFactory = capnp::ByteStreamFactory;
using EventDispatcherClient = workerd::rpc::EventDispatcher::Client;

// Type aliases for WorkerInterface types
using CustomEvent = workerd::WorkerInterface::CustomEvent;

}  // namespace workerd::rust::worker