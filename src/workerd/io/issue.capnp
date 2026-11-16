# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xdd8a9e1ab1998110;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::issues");

using import "/workerd/io/outcome.capnp".EventOutcome;

# One issue the runtime found in a customer's Worker, as the embedder's IssueSink receives it at
# the end of the request. Field names are part of the customer contract.

struct Issue {
  code @0 :Text;
  # A stable string, such as "uncaught_exception". Titles and severity live downstream, keyed by it.

  message @1 :Text;
  fields @2 :List(Field);
  errors @3 :List(Error);
  attachments @4 :List(Attachment);

  breadcrumbs @5 :List(Breadcrumb);
  # What the request did just before the issue was reported, oldest first.

  user @6 :User;
  invocation @7 :Invocation;

  status @8 :UInt16;
  # The HTTP status the request returned, or 0 if none was reported.

  outcome @9 :EventOutcome;
  # What workerd knew of the request's outcome when it ended.

  struct Field {
    name @0 :Text;
    value :union {
      text @1 :Text;
      int @2 :Int64;
      float @3 :Float64;
      flag @4 :Bool;
    }
  }

  struct Error {
    name @0 :Text;
    message @1 :Text;
    stack @2 :Text;
    # Empty if the error has no stack.
  }

  struct Attachment {
    name @0 :Text;
    body @1 :Text;
    truncated @2 :Bool;
  }

  struct Breadcrumb {
    atMs @0 :UInt32;
    # Milliseconds after the collector was created.
    kind @1 :Kind;
    state @2 :State;
    level @3 :Text;
    # The log level. Empty for a call.
    label @4 :Text;

    enum Kind {
      call @0;
      log @1;
    }

    enum State {
      started @0;
      done @1;
      failed @2;
      none @3;
      # Logs have no state.
    }
  }

  struct User {
    userId @0 :Text;
    sessionId @1 :Text;
    accountId @2 :Text;
    # Empty if never set.
  }

  struct Invocation {
    event @0 :Text;
    # The kind of event, such as "fetch".
    method @1 :Text;
    path @2 :Text;
  }
}
