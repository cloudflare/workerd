#pragma once

#include <workerd/io/outcome.capnp.h>
#include <workerd/io/worker-interface.capnp.h>

namespace workerd {

using LogLevel = rpc::Trace::Log::Level;
using ExecutionModel = rpc::Trace::ExecutionModel;

enum class PipelineLogLevel {
  // WARNING: This must be kept in sync with PipelineDef::LogLevel (which is not in the OSS
  //   release).
  NONE,
  FULL
};

namespace trace {

// Metadata describing the onset of a trace session.
struct OnsetInfo {
  kj::Maybe<kj::String> ownerId = kj::none;
  kj::Maybe<kj::String> stableId = kj::none;
  kj::Maybe<kj::String> scriptName = kj::none;
  kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion = kj::none;
  kj::Maybe<kj::String> dispatchNamespace = kj::none;
  kj::Maybe<kj::String> scriptId = kj::none;
  kj::Array<kj::String> scriptTags = nullptr;
  kj::Maybe<kj::String> entrypoint = kj::none;
  ExecutionModel ExecutionModel;
};

}  // namespace trace
}  // namespace workerd
