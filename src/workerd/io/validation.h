// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/array.h>
#include <kj/common.h>
#include <kj/string.h>

namespace workerd {

// Interface for reporting errors encountered while validating a Worker's configuration (e.g.
// compatibility flags, exported entrypoints, Durable Object classes).
class ValidationErrorReporter {
 public:
  virtual void addError(kj::String error) = 0;

  // Report that the Worker implements a stateless entrypoint (e.g. WorkerEntrypoint or plain
  // object export) with the given export name and methods.
  virtual void addEntrypoint(
      kj::Maybe<kj::StringPtr> exportName, kj::Array<kj::String> methods) = 0;

  // Report that the Worker exports a Durable Object class with the given name.
  virtual void addActorClass(kj::StringPtr exportName) = 0;

  // Report that the Worker exports a Workflow class with the given name.
  virtual void addWorkflowClass(kj::StringPtr exportName, kj::Array<kj::String> methods) = 0;
};

}  // namespace workerd
