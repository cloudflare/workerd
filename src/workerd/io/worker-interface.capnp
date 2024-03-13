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
using import "/capnp/compat/byte-stream.capnp".ByteStream;
using import "/workerd/io/outcome.capnp".EventOutcome;
using import "/workerd/io/script-version.capnp".ScriptVersion;

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
  scriptVersion @19 :ScriptVersion;

  eventTimestampNs @5 :Int64;

  eventInfo :union {
    none @3 :Void;
    fetch @6 :FetchEventInfo;
    scheduled @7 :ScheduledEventInfo;
    alarm @9 :AlarmEventInfo;
    queue @15 :QueueEventInfo;
    custom @13 :CustomEventInfo;
    email @16 :EmailEventInfo;
    trace @18 :TraceEventInfo;
    hibernatableWebSocket @20 :HibernatableWebSocketEventInfo;
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

  struct TraceEventInfo {
    struct TraceItem {
      scriptName @0 :Text;
    }

    traces @0 :List(TraceItem);
  }

  struct HibernatableWebSocketEventInfo {
    type :union {
      message @0 :Void;
      close :group {
        code @1 :UInt16;
        wasClean @2 :Bool;
      }
      error @3 :Void;
    }
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

  diagnosticChannelEvents @17 :List(DiagnosticChannelEvent);
  struct DiagnosticChannelEvent {
    timestampNs @0 :Int64;
    channel @1 :Text;
    message @2 :Data;
  }
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

struct QueueMessage @0x944adb18c0352295 {
  id @0 :Text;
  timestampNs @1 :Int64;
  data @2 :Data;
  contentType @3 :Text;
}

struct QueueRetryBatch {
  retry @0 :Bool;
  union {
    undefined @1 :Void;
    delaySeconds @2 :Int32;
  }
}

struct QueueRetryMessage {
  msgId @0 :Text;
  union {
    undefined @1 :Void;
    delaySeconds @2 :Int32;
  }
}

struct QueueResponse @0x90e98932c0bfc0de {
  outcome @0 :EventOutcome;
  ackAll @1 :Bool;
  retryBatch @2 :QueueRetryBatch;
  # Retry options for the batch.
  explicitAcks @3 :List(Text);
  # List of Message IDs that were explicitly marked as acknowledged.
  retryMessages @4 :List(QueueRetryMessage);
  # List of retry options for messages that were explicitly marked for retry.
}

struct HibernatableWebSocketEventMessage {
  payload :union {
    text @0 :Text;
    data @1 :Data;
    close :group {
      code @2 :UInt16;
      reason @3 :Text;
      wasClean @4 :Bool;
    }
    error @5 :Text;
    # TODO(someday): This could be an Exception instead of Text.
  }
  websocketId @6: Text;
  eventTimeoutMs @7: UInt32;
}

struct HibernatableWebSocketResponse {
  outcome @0 :EventOutcome;
}

interface HibernatableWebSocketEventDispatcher {
  hibernatableWebSocketEvent @0 (message: HibernatableWebSocketEventMessage )
      -> (result :HibernatableWebSocketResponse);
  # Run a hibernatable websocket event
}

enum SerializationTag {
  # Tag values for all serializable types supported by the Workers API.

  invalid @0;
  # Not assigned to anything. Reserved to make things less weird if a zero-valued tag gets written
  # by accident.

  jsRpcStub @1;

  writableStream @2;
  readableStream @3;

  headers @4;
  request @5;
}

enum StreamEncoding {
  # Specifies the internal content-encoding of a ReadableStream or WritableStream. This serves an
  # optimization which is not visible to the app: if we end up hooking up streams so that a source
  # is pumped to a sink that has the same encoding, we can avoid a decompression/recompression
  # round trip. However, if the application reads/writes raw bytes, then we must decode/encode
  # them under the hood.

  identity @0;
  gzip @1;
  brotli @2;
}

interface Handle {
  # Type with no methods, but something happens when you drop it.
}

struct JsValue {
  # A serialized JavaScript value being passed over RPC.

  v8Serialized @0 :Data;
  # JS value that has been serialized for network transport.

  externals @1 :List(External);
  # The serialized data may contain "externals" -- references to external resources that cannot
  # simply be serialized. If so, they are placed in this separate list of externals.
  #
  # (We could also call these "capabilities", but that word is pretty overloaded already.)

  struct External {
    union {
      invalid @0 :Void;
      # Invalid default value to reduce confusion if an External wasn't initialized properly.
      # This should never appear in a real JsValue.

      rpcTarget @1 :JsRpcTarget;
      # An object that can be called over RPC.

      writableStream :group {
        # A WritableStream. This is much easier to represent that ReadableStream because the bytes
        # flow from the receiver to the sender, and therefore a round trip is obviously necessary
        # before the bytes can begin flowing.

        byteStream @2 :ByteStream;
        encoding @3 :StreamEncoding;
      }

      readableStream :group {
        # A ReadableStream. The sender of the JsValue will use the associated StreamSink to open a
        # stream of type `ByteStream`.

        encoding @4 :StreamEncoding;
        # Bytes read from the stream have this encoding.

        expectedLength :union {
          unknown @5 :Void;
          known @6 :UInt64;
        }
      }

      # TODO(soon): WebSocket, Request, Response
    }
  }

  interface StreamSink {
    # A JsValue may contain streams that flow from the sender to the receiver. We don't want such
    # streams to require a network round trip before the stream can begin pumping. So, we need a
    # place to start sending bytes right away.
    #
    # To that end, JsRpcTarget::call() returns a `paramsStreamSink`. Immediately upon sending the
    # request, the client can use promise pipelining to begin pushing bytes to this object.
    #
    # Similarly, the caller passes a `resultsStreamSink` to the callee. If the response contains
    # any streams, it can start pushing to this immediately after responding.

    startStream @0 (externalIndex :UInt32) -> (stream :Capability);
    # Opens a stream corresponding to the given index in the JsValue's `externals` array. The type
    # of capability returned depends on the type of external. E.g. for `readableStream`, it is a
    # `ByteStream`.
  }
}

interface JsRpcTarget $Cxx.allowCancellation {
  struct CallParams {
    union {
      methodName @0 :Text;
      # Equivalent to `methodPath` where the list has only one element equal to this.

      methodPath @2 :List(Text);
      # Path of properties to follow from the JsRpcTarget itself to find the method being called.
      # E.g. if the application does:
      #
      #     myRpcTarget.foo.bar.baz()
      #
      # Then the path is ["foo", "bar", "baz"].
      #
      # The path can also be empty, which means that the JsRpcTarget itself is being invoked as a
      # function.
    }

    operation :union {
      callWithArgs @1 :JsValue;
      # Call the property as a function. This is a JsValue that always encodes a JavaScript Array
      # containing the arguments to the call.
      #
      # If `callWithArgs` is null (but is still the active member of the union), this indicates
      # that the argument list is empty.

      getProperty @3 :Void;
      # This indicates that we are not actually calling a method at all, but rather retrieving the
      # value of a property. RPC classes are allowed to define properties that can be fetched
      # asynchronously, although more commonly properties will be RPC targets themselves and their
      # methods will be invoked by sending a `methodPath` with more than one element. That is,
      # imagine you have:
      #
      #     myRpcTarget.foo.bar();
      #
      # This code makes a single RPC call with a path of ["foo", "bar"]. However, you could also
      # write:
      #
      #     let foo = await myRpcTarget.foo;
      #     foo.bar();
      #
      # This will make two separate calls. The first call is to "foo" and `getProperty` is used.
      # This returns a new JsRpcTarget. The second call is on that target, invoking the method
      # "bar".
    }

    resultsStreamSink @4 :JsValue.StreamSink;
    # StreamSink used for ReadableStreams found in the results.
  }

  struct CallResults {
    result @0 :JsValue;
    # The returned value.

    callPipeline @1 :JsRpcTarget;
    # Enables promise pipelining on the eventual call result. This is a JsRpcTarget wrapping the
    # result of the call, even if the result itself is a serializable object that would not
    # normally be treated as an RPC target. The caller may use this to initiate speculative calls
    # on this result without waiting for the initial call to complete (using promise pipelining).

    hasDisposer @2 :Bool;
    # If `hasDisposer` is true, the server side returned a serializable object (not a stub) with a
    # disposer (Symbol.dispose). The disposer itself is not included in the object's serialization,
    # but dropping the `callPipeline` will invoke it.
    #
    # On the client side, when an RPC returns a plain object, a disposer is added to it. In order
    # to avoid confusion, we want the server-side disposer to be invoked only after the client-side
    # disposer is invoked. To that end, when `hasDisposer` is true, the client should hold on to
    # `callPipeline` until the disposer is invoked. If `hasDisposer` is false, `callPipeline` can
    # safely be dropped immediately.

    paramsStreamSink @3 :JsValue.StreamSink;
    # StreamSink used for ReadableStreams found in the params. The caller begins sending bytes for
    # these streams immediately using promise pipelining.
  }

  call @0 CallParams -> CallResults;
  # Runs a Worker/DO's RPC method.
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


  runAlarm @4 (scheduledTime :Int64, retryCount :UInt32) -> (result :AlarmRun);
  # Runs a worker's alarm.
  # scheduledTime is a unix timestamp in milliseconds for when the alarm should be run
  # retryCount indicates the retry count, if it's a retry. Else it'll be 0.
  # Returns an AlarmRun, detailing information about the run such as
  # the outcome and whether the run should be retried. This does not complete immediately.
  #
  # TODO(cleanup): runAlarm()'s implementation currently relies on *not* allowing cancellation.
  #   It would be cleaner to handle that inside the implementation so we could mark the entire
  #   interface (and file) with allowCancellation.

  queue @8 (messages :List(QueueMessage), queueName :Text) -> (result :QueueResponse)
      $Cxx.allowCancellation;
  # Delivers a batch of queue messages to a worker's queue event handler. Returns information about
  # the success of the batch, including which messages should be considered acknowledged and which
  # should be retried.

  jsRpcSession @9 () -> (topLevel :JsRpcTarget) $Cxx.allowCancellation;
  # Opens a JS rpc "session". The call does not return until the session is complete.
  #
  # `topLevel` is the top-level RPC target, on which exactly one method call can be made. This
  # call must be made using pipelining since `jsRpcSession()` won't return until after the call
  # completes.
  #
  # If, through the one top-level call, new capabilities are exchanged between the client and
  # server, then `jsRpcSession()` won't return until all those capabilities have been dropped.
  #
  # In C++, we use `WorkerInterface::customEvent()` to dispatch this event.

  obsolete5 @5();
  obsolete6 @6();
  obsolete7 @7();
  # Deleted methods, do not reuse these numbers.

  # Other methods might be added to handle other kinds of events, e.g. TCP connections, or maybe
  # even native Cap'n Proto RPC eventually.
}

interface WorkerdBootstrap {
  # Bootstrap interface exposed by workerd when serving Cap'n Proto RPC.

  startEvent @0 () -> (dispatcher :EventDispatcher);
  # Start a new event. Exactly one event should be delivered to the returned EventDispatcher.
  #
  # TODO(someday): Pass cfBlobJson? Currently doesn't matter since the cf blob is only present for
  #   HTTP requests which can be delivered over regular HTTP instead of capnp.
}
