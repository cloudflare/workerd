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
using import "/workerd/io/trace.capnp".UserSpanData;

struct InvocationSpanContext {
  struct TraceId {
    high @0 :UInt64;
    low @1 :UInt64;
  }
  traceId @0 :TraceId;
  invocationId @1 :TraceId;
  spanId @2 :UInt64;
}

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

  spans @26 :List(UserSpanData);

  exceptions @1 :List(Exception);
  struct Exception {
    timestampNs @0 :Int64;
    name @1 :Text;
    message @2 :Text;
    stack @3 :Text;
  }

  outcome @2 :EventOutcome;
  scriptName @4 :Text;
  scriptVersion @19 :ScriptVersion;
  scriptId @23 :Text;

  eventTimestampNs @5 :Int64;

  eventInfo :union {
    none @3 :Void;
    fetch @6 :FetchEventInfo;
    jsRpc @21 :JsRpcEventInfo;
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

  struct JsRpcEventInfo {
    methodName @0 :Text;
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

  entrypoint @22 :Text;

  diagnosticChannelEvents @17 :List(DiagnosticChannelEvent);
  struct DiagnosticChannelEvent {
    timestampNs @0 :Int64;
    channel @1 :Text;
    message @2 :Data;
  }

  truncated @24 :Bool;
  # Indicates that the trace was truncated due to reaching the maximum size limit.

  enum ExecutionModel {
    stateless @0;
    durableObject @1;
    workflow @2;
  }
  executionModel @25 :ExecutionModel;
  # the execution model of the worker being traced. Can be stateless for a regular worker,
  # durableObject for a DO worker or workflow for the upcoming Workflows feature.

  # =====================================================================================
  # Additional types for streaming tail workers

  struct Attribute {
    # An Attribute mark is used to add detail to a span over its lifetime.
    # The Attribute struct can also be used to provide arbitrary additional
    # properties for some other structs.
    # Modeled after https://opentelemetry.io/docs/concepts/signals/traces/#attributes
    struct Value {
      inner :union {
        text @0 :Text;
        bool @1 :Bool;
        int @2 :Int64;
        float @3 :Float64;
      }
    }
    name @0 :Text;
    value @1 :List(Value);
  }

  struct Return {
    # A Return mark is used to mark the point at which a span operation returned
    # a value. For instance, when a fetch subrequest response is received, or when
    # the fetch handler returns a Response. Importantly, it does not signal that the
    # span has closed, which may not happen for some period of time after the return
    # mark is recorded (e.g. due to things like waitUntils or waiting to fully ready
    # the response body payload, etc). Not all spans will have a Return mark.
    info :union {
      empty @0 :Void;
      custom @1 :List(Attribute);
      fetch @2 :FetchResponseInfo;
    }
  }

  struct SpanOpen {
    # Marks the opening of a child span within the streaming tail session.
    operationName @0 :Text;
    info :union {
      empty @1 :Void;
      custom @2 :List(Attribute);
      fetch @3 :FetchEventInfo;
      jsRpc @4 :JsRpcEventInfo;
    }
  }

  struct SpanClose {
    # Marks the closing of a child span within the streaming tail session.
    # Once emitted, no further mark events should occur within the closed
    # span.
    outcome @0 :EventOutcome;
  }

  struct Resume {
    # A resume event indicates that we are resuming a previously hibernated
    # tail session.

    attachment @0 :Data;
    # When a tail session is hibernated, the tail worker is given the opportunity
    # to provide some additional data that will be serialized and stored with the
    # hibernated state. When the stream is resumed, if the tail worker has provided
    # such data, it will be passed back to the worker in the resume event.
  }

  struct Onset {
    # The Onset and Outcome event types are special forms of SpanOpen and
    # SpanClose that explicitly mark the start and end of the root span.
    # A streaming tail session will always begin with an Onset event, and
    # always end with an Outcome event.
    executionModel @0 :ExecutionModel;
    scriptName @1 :Text;
    scriptVersion @2 :ScriptVersion;
    dispatchNamespace @3 :Text;
    scriptTags @4 :List(Text);
    entryPoint @5 :Text;

    trigger @6 :InvocationSpanContext;
    # If this invocation was triggered by a different invocation that
    # is being traced, the trigger will identify the triggering span.
    # Propagation of the trigger context is not required, and in some
    # cases is not desirable.

    struct Info { union {
      fetch @0 :FetchEventInfo;
      jsRpc @1 :JsRpcEventInfo;
      scheduled @2 :ScheduledEventInfo;
      alarm @3 :AlarmEventInfo;
      queue @4 :QueueEventInfo;
      email @5 :EmailEventInfo;
      trace @6 :TraceEventInfo;
      hibernatableWebSocket @7 :HibernatableWebSocketEventInfo;
      resume @8 :Resume;
      custom @9 :CustomEventInfo;
    }
    }
    info @7: Info;
  }

  struct Outcome {
    outcome @0 :EventOutcome;
    cpuTime @1 :UInt64;
    wallTime @2 :UInt64;
  }

  struct Hibernate {
    # A hibernate event indicates that the tail session is being hibernated.
  }

  struct Link {
    # A link to another invocation span context.
    label @0 :Text;
    context @1 :InvocationSpanContext;
  }

  struct TailEvent {
    # A streaming tail worker receives a series of Tail Events. Tail events always
    # occur within an InvocationSpanContext. The first TailEvent delivered to a
    # streaming tail session is always an Onset. The final TailEvent delivered is
    # always an Outcome or Hibernate. Between those can be any number of SpanOpen,
    # SpanClose, and Mark events. Every SpanOpen *must* be associated with a SpanClose
    # unless the stream was abruptly terminated.
    context @0 :InvocationSpanContext;
    timestampNs @1 :Int64;
    sequence @2 :UInt32;
    event :union {
      onset @3 :Onset;
      outcome @4 :Outcome;
      hibernate @5 :Hibernate;
      spanOpen @6 :SpanOpen;
      spanClose @7 :SpanClose;
      attribute @8 :List(Attribute);
      return @9 :Return;
      diagnosticChannelEvent @10 :DiagnosticChannelEvent;
      exception @11 :Exception;
      log @12 :Log;
      link @13 :Link;
      # While invocation span context (EW-8821) is not fully implemented, send completed spans as
      # events so that we can provide timestamps and parent span definitions properly. Can be
      # removed once that is done and span API is finalized.
      completedSpan @14 :UserSpanData;
    }
  }
}

struct SendTracesRun @0xde913ebe8e1b82a5 {
  outcome @0 :EventOutcome;
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
  attempts @4 :UInt16;
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
  response @6;

  domException @7;
  domExceptionV2 @8;
  # Keep this value in sync with the DOMException::SERIALIZATION_TAG in
  # /src/workerd/jsg/dom-exception (but we can't actually change this value
  # without breaking things).

  abortSignal @9;
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

      abortTrigger @7 :Void;
      # Indicates that an `AbortTrigger` is being passed, see the `AbortTrigger` interface for the
      # mechanism used to trigger the abort later. This is modeled as a stream, since the sender is
      # the one that will later on send the abort signal. This external will have an associated
      # stream in the corresponding `StreamSink` with type `AbortTrigger`.

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

interface AbortTrigger $Cxx.allowCancellation {
  # When an `AbortSignal` is sent over RPC, the sender initiates a "stream" with this RPC interface
  # type which is later used to signal the abort. This is not really a "stream", since only one
  # message is sent. But it makes sense to model this way because the message is sent in the same
  # direction as the `JsValue` that originally transmitted the `AbortSignal` object.
  # When an `AbortSignal` is serialized, the original signal is the client, and the deserialized
  # clone is the server.

  abort @0 (reason :JsValue) -> ();
  # Allows a cloned abort signal to be triggered over RPC when the original signal is triggered.
  # `reason` is an arbitrary JavaScript value which will appear in the resulting `AbortError`s.

  release @1 () -> ();
  # Informs a cloned signal that the original signal is being destroyed, and the abort will never
  # be triggered. Otherwise, the cloned signal will treat a dropped cabability as an abort.
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

interface TailStreamTarget $Cxx.allowCancellation {
  # Interface used to deliver streaming tail events to a tail worker.
  struct TailStreamParams {
    events @0 :List(Trace.TailEvent);
  }

  struct TailStreamResults {
    stop @0 :Bool;
    # For an initial tailStream call, the stop flag indicates that the tail worker does
    # not wish to continue receiving events. If the stop field is not set, or the value
    # is false, then events will be delivered to the tail worker until stop is indicated.
  }

  report @0 TailStreamParams -> TailStreamResults;
  # Report one or more streaming tail events to a tail worker.
}

interface EventDispatcher @0xf20697475ec1752d {
  # Interface used to deliver events to a Worker's global event handlers.

  getHttpService @0 () -> (http :HttpService) $Cxx.allowCancellation;
  # Gets the HTTP interface to this worker (to trigger FetchEvents).

  sendTraces @1 (traces :List(Trace)) -> (result :SendTracesRun) $Cxx.allowCancellation;
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

  tailStreamSession @10 () -> (topLevel :TailStreamTarget) $Cxx.allowCancellation;
  # Opens a streaming tail session. The call does not return until the session is complete.
  #
  # `topLevel` is the top-level tail session target, on which exactly one method call can
  # be made. This call must be made using pipelining since `tailStreamSession()` won't return
  # until after the call completes.

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
