// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Runtime-native issues. Code that sees a problem in a customer's Worker reports it to the
// request's IssueCollector with a stable string code. The collector, implemented in Rust
// (//src/workerd/io/issues), holds what is reported until the request ends, then writes each issue
// it kept to the embedder's IssueSink as an issue.capnp record.
//
// The embedder opts in per request by owning an IssueCollector and returning it from
// RequestObserver::getIssueCollector(). It ends the collector from the observer's destructor, which
// runs after everything that can report for the request, and passes the outcome it knows.
// Without a collector every feed point costs one check, and nothing is built.
//
// A code is a plain string; there is no registry. Each team keeps its codes as constants in its own
// code, like span names. The collector knows three by name: the fallbacks ERROR_OUTCOME and
// HTTP_SERVER_ERROR, which are dropped if a fact was reported before them, and ERROR_LOG, which it
// caps.

#include <workerd/io/issue.capnp.h>
#include <workerd/io/outcome.capnp.h>

#include <rust/cxx.h>

#include <kj/function.h>
#include <kj/one-of.h>
#include <kj/string.h>

namespace workerd::issues {

struct Collector;

constexpr kj::StringPtr UNCAUGHT_EXCEPTION = "uncaught_exception"_kj;
constexpr kj::StringPtr ERROR_LOG = "error_log"_kj;
constexpr kj::StringPtr HTTP_SERVER_ERROR = "http_server_error"_kj;
constexpr kj::StringPtr ERROR_OUTCOME = "error_outcome"_kj;

// A structured fact about an issue.
struct Field {
  kj::StringPtr name;
  kj::OneOf<kj::StringPtr, int64_t, double, bool> value;
};

// An Error the Worker threw or logged.
struct Error {
  kj::StringPtr name;
  kj::StringPtr message;
  kj::Maybe<kj::StringPtr> stack;
};

// Detail that is costly to build. `render` runs only if the issue is kept, so an issue that is
// capped or repeated never pays for it.
struct Attachment {
  kj::StringPtr name;
  kj::Function<kj::String()> render;
};

enum class BreadcrumbKind : uint8_t {
  CALL_STARTED,
  CALL_FINISHED_OK,
  CALL_FINISHED_FAILED,
  LOG,
};

// Receives each issue a request kept, when the request ends.
class IssueSink {
 public:
  virtual void write(Issue::Reader issue) = 0;
};

// One per request that opted in. Used only on the request's thread.
class IssueCollector {
 public:
  explicit IssueCollector(IssueSink& sink);
  ~IssueCollector() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(IssueCollector);

  // Holds an issue for the end of the request. Returns false if it was dropped, capped or an exact
  // repeat, in which case the arguments are not read and no attachment is rendered.
  bool report(kj::StringPtr code,
      kj::StringPtr message,
      kj::ArrayPtr<const Field> fields = nullptr,
      kj::ArrayPtr<const Error> errors = nullptr,
      kj::ArrayPtr<Attachment> attachments = nullptr);

  // Records something that happened shortly before a possible issue. For LOG, `detail` is the log
  // level name and `label` is the start of the first string argument. For calls, `detail` is empty
  // and `label` is the operation name. Only CALL_STARTED and LOG are recorded.
  void breadcrumb(BreadcrumbKind kind, kj::StringPtr detail, kj::StringPtr label);

  // Notes who the request is for, from the user.id, session.id and account.id span attributes.
  void noteUserContext(kj::StringPtr key, kj::StringPtr value);

  void setEvent(kj::StringPtr event);
  void setRequest(kj::StringPtr method, kj::StringPtr path);

  // Records the HTTP status the request is returning.
  void setStatus(uint32_t status);

  // Writes every held issue to the sink with the request's status and `outcome`. A second call
  // writes nothing. A sink that throws loses that issue and is logged.
  void end(EventOutcome outcome);

 private:
  IssueSink& sink;
  ::rust::Box<Collector> impl;
};

}  // namespace workerd::issues
