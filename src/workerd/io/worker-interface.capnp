# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xf7958855f6746344;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::rpc");
# We do not use `$Cxx.allowCancellation` because runAlarm() currently depends on blocking
# cancellation.

using import "/capnp/compat/http-over-capnp.capnp".HttpMethod;
using import "/capnp/compat/http-over-capnp.capnp".HttpService;
using import "/workerd/io/outcome.capnp".EventOutcome;

struct Trace @0x8e8d911203762d34 {
  logs @0 :List(Log);
  struct Log {
    timestampNs @0 :Int64;

    logLevel @1 :Level;
    enum Level {
      debug @0 $Cxx.name("debug_");  # avoid collision with macro on Apple platforms
      info @1;
      log @2;
      warn @3;
      error @4;
    }

    message @2 :Text;
  }

  exceptions @1 :List(Exception);
  struct Exception {
    timestampNs @0 :Int64;
    name @1 :Text;
    message @2 :Text;
  }

  outcome @2 :EventOutcome;
  scriptName @4 :Text;

  eventTimestampNs @5 :Int64;

  eventInfo :union {
    none @3 :Void;
    fetch @6 :FetchEventInfo;
    scheduled @7 :ScheduledEventInfo;
    alarm @9 :AlarmEventInfo;
    queue @15 :QueueEventInfo;
    custom @13 :CustomEventInfo;
    email @16 :EmailEventInfo;
  }
  struct FetchEventInfo {
    method @0 :HttpMethod;
    url @1 :Text;
    cfJson @2 :Text;
    # Empty string indicates missing cf blob
    headers @3 :List(Header);
    struct Header {
      name @0 :Text;
      value @1 :Text;
    }
  }

  struct ScheduledEventInfo {
    scheduledTime @0 :Float64;
    cron @1 :Text;
  }

  struct AlarmEventInfo {
    scheduledTimeMs @0 :Int64;
  }

  struct QueueEventInfo {
    queueName @0 :Text;
    batchSize @1 :UInt32;
  }

  struct EmailEventInfo {
    mailFrom @0 :Text;
    rcptTo @1 :Text;
    rawSize @2 :UInt32;
  }

  struct CustomEventInfo {}

  response @8 :FetchResponseInfo;
  struct FetchResponseInfo {
    statusCode @0 :UInt16;
  }

  cpuTime @10 :UInt64;
  wallTime @11 :UInt64;

  dispatchNamespace @12 :Text;
  scriptTags @14 :List(Text);
}

struct ScheduledRun @0xd98fc1ae5c8095d0 {
  outcome @0 :EventOutcome;

  retry @1 :Bool;
}

struct AlarmRun @0xfa8ea4e97e23b03d {
  outcome @0 :EventOutcome;

  retry @1 :Bool;
  retryCountsAgainstLimit @2 :Bool = true;
}

interface EventDispatcher @0xf20697475ec1752d {
  # Interface used to deliver events to a Worker's global event handlers.

  getHttpService @0 () -> (http :HttpService) $Cxx.allowCancellation;
  # Gets the HTTP interface to this worker (to trigger FetchEvents).

  sendTraces @1 (traces :List(Trace)) $Cxx.allowCancellation;
  # Deliver a trace event to a trace worker. This always completes immediately; the trace handler
  # runs as a "waitUntil" task.

  prewarm @2 (url :Text) $Cxx.allowCancellation;

  runScheduled @3 (scheduledTime :Int64, cron :Text) -> (result :ScheduledRun)
      $Cxx.allowCancellation;
  # Runs a scheduled worker. Returns a ScheduledRun, detailing information about the run such as
  # the outcome and whether the run should be retried. This does not complete immediately.


  runAlarm @4 (scheduledTime :Int64) -> (result :AlarmRun);
  # Runs a worker's alarm.
  # scheduledTime is a unix timestamp in milliseconds for when the alarm should be run
  # Returns an AlarmRun, detailing information about the run such as
  # the outcome and whether the run should be retried. This does not complete immediately.
  #
  # TODO(cleanup): runAlarm()'s implementation currently relies on *not* allowing cancellation.
  #   It would be cleaner to handle that inside the implementation so we could mark the entire
  #   interface (and file) with allowCancellation.

  obsolete5 @5();
  obsolete6 @6();
  obsolete7 @7();
  # Deleted methods, do not reuse these numbers.

  # Other methods might be added to handle other kinds of events, e.g. TCP connections, or maybe
  # even native Cap'n Proto RPC eventually.
}
