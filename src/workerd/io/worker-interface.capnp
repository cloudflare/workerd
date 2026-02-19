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
using import "/workerd/io/trace.capnp".TagValue;
using import "/workerd/io/trace.capnp".UserSpanData;
using import "/workerd/io/frankenvalue.capnp".Frankenvalue;

# A 128-bit trace ID used to identify traces.
struct TraceId {
  high @0 :UInt64;
  low @1 :UInt64;
}

# InvocationSpanContext used to identify the current tracing context. Only used internally so far.
struct InvocationSpanContext {
  # The 128-bit ID uniquely identifying a trace.
  traceId @0 :TraceId;
  # The 128-bit ID identifying a worker stage invocation within a trace.
  invocationId @1 :TraceId;
  # The 64-bit span ID identifying an individual span within a worker stage invocation.
  spanId @2 :UInt64;
}

# Span context for a tail event – this is provided for each tail event.
struct SpanContext {
  # The 128-bit ID uniquely identifying a trace.
  traceId @0 :TraceId;
  # spanId in which this event is handled
  # for Onset and SpanOpen events this would be the parent span id
  # for Outcome and SpanClose these this would be the span id of the opening Onset and SpanOpen events
  # For Hibernate and Mark this would be the span under which they were emitted.
  # This is only empty if:
  #  1. This is an Onset event
  #  2. We are not inheriting any SpanContext. (e.g. this is a cross-account service binding or a new top-level invocation)
  info :union {
    empty @1 :Void;
    spanId @2 :UInt64;
  }
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

  obsolete26 @26 :List(UserSpanData);
  # spans are unavailable in full trace objects.

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
  durableObjectId @27 :Text;

  diagnosticChannelEvents @17 :List(DiagnosticChannelEvent);
  struct DiagnosticChannelEvent {
    timestampNs @0 :Int64;
    channel @1 :Text;
    message @2 :Data;
  }

  # Indicates how many tail stream events were dropped in total.
  struct DroppedEvents {
    count @0 :UInt32;
  }

  struct StreamDiagnosticsEvent {
    # In the future, we plan to support several types of events here, for now only the dropped
    # events diagnostic is supported.
    diagnostic :union {
      undefined @0 :Void;
      droppedEvents @1 :DroppedEvents;
    }
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
    name @0 :Text;
    value @1 :List(TagValue);
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
      fetch @1 :FetchResponseInfo;
    }
  }

  struct SpanOpen {
    # Marks the opening of a child span within the streaming tail session.
    operationName @0 :Text;
    spanId @1 :UInt64;
    # id for the span being opened by this SpanOpen event.
    info :union {
      empty @2 :Void;
      custom @3 :List(Attribute);
      fetch @4 :FetchEventInfo;
      jsRpc @5 :JsRpcEventInfo;
    }
  }

  struct SpanClose {
    # Marks the closing of a child span within the streaming tail session.
    # Once emitted, no further mark events should occur within the closed
    # span.
    outcome @0 :EventOutcome;
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
    scriptId @4 :Text;
    scriptTags @5 :List(Text);
    entryPoint @6 :Text;

    struct Info { union {
      fetch @0 :FetchEventInfo;
      jsRpc @1 :JsRpcEventInfo;
      scheduled @2 :ScheduledEventInfo;
      alarm @3 :AlarmEventInfo;
      queue @4 :QueueEventInfo;
      email @5 :EmailEventInfo;
      trace @6 :TraceEventInfo;
      hibernatableWebSocket @7 :HibernatableWebSocketEventInfo;
      custom @8 :CustomEventInfo;
    }
    }
    info @7: Info;
    spanId @8: UInt64;
    # id for the span being opened by this Onset event.
    attributes @9 :List(Attribute);
  }

  struct Outcome {
    outcome @0 :EventOutcome;
    cpuTime @1 :UInt64;
    wallTime @2 :UInt64;
  }

  struct TailEvent {
    # A streaming tail worker receives a series of Tail Events. Tail events always occur within an
    # InvocationSpanContext. The first TailEvent delivered to a streaming tail session is always an
    # Onset. The final TailEvent delivered is always an Outcome. Between those can be any number of
    # SpanOpen, SpanClose, and Mark events. Every SpanOpen *must* be associated with a SpanClose
    # unless the stream was abruptly terminated.
    # Inherited spanContext for this event.
    spanContext @0: SpanContext;
    # invocation id of the currently invoked worker stage.
    # invocation id will always be unique to every Onset event and will be the same until the Outcome event.
    invocationId @1: TraceId;
    # time for the tail event. This will be provided as I/O time from the perspective of the tail worker.
    timestampNs @2 :Int64;
    # unique sequence identifier for this tail event, starting at zero.
    sequence @3 :UInt32;
    event :union {
      onset @4 :Onset;
      outcome @5 :Outcome;
      spanOpen @6 :SpanOpen;
      spanClose @7 :SpanClose;
      attribute @8 :List(Attribute);
      return @9 :Return;
      diagnosticChannelEvent @10 :DiagnosticChannelEvent;
      exception @11 :Exception;
      log @12 :Log;
      streamDiagnostics @13 :StreamDiagnosticsEvent;
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

  nativeError @10;
  # A JavaScript native error, such as Error, TypeError, etc. These are typically
  # not handled as host objects in V8 but we handle them as such in workers in
  # order to preserve additional information that we may attach to them.

  serviceStub @11;
  # A ServiceStub aka Fetcher aka Service Binding.
  #
  # Such stubs are different from jsRpcStub in that they don't point to a single live object, but
  # instead represent a service that can be instantiated anywhere. This means that they can be
  # passed around the world and instantiated in a different location, as well as persisted in
  # long-term storage.
  #
  # Also because of all this, service stubs can be embedded in the `env` and `ctx.props` of other
  # Workers. Regular RPC stubs cannot.

  actorClass @12;
  # An actor class reference, aka DurableObjectClass. Can be used to instantiate a facet.
  #
  # Similar to serviceStub, this refers to the entrypoint of a Worker that can be instantiated
  # anywhere and any time, and thus can be persisted and used in `env` and `ctx.props`, etc.
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

        stream @10 :ExternalPusher.InputStream;
        # If present, a stream pushed using the destination isolate's ExternalPusher.
        #
        # If null (deprecated), then the sender will use the associated StreamSink to open a stream
        # of type `ByteStream`. StreamSink is in the process of being replaced by ExternalPusher.

        encoding @4 :StreamEncoding;
        # Bytes read from the stream have this encoding.

        expectedLength :union {
          # NOTE: This is obsolete when `stream` is set. Instead, the length is passed to
          #   ExternalPusher.pushByteStream().

          unknown @5 :Void;
          known @6 :UInt64;
        }
      }

      abortTrigger @7 :Void;
      # Indicates that an `AbortTrigger` is being passed, see the `AbortTrigger` interface for the
      # mechanism used to trigger the abort later. This is modeled as a stream, since the sender is
      # the one that will later on send the abort signal. This external will have an associated
      # stream in the corresponding `StreamSink` with type `AbortTrigger`.
      #
      # TODO(soon): This will be obsolete when we stop using `StreamSink`; `abortSignal` will
      #   replace it. (The name is wrong anyway -- this is the signal end, not the trigger end.)

      abortSignal @11 :ExternalPusher.AbortSignal;
      # Indicates that an `AbortSignal` is being passed.

      subrequestChannelToken @8 :Data;
      actorClassChannelToken @9 :Data;
      # Encoded ChannelTokens. See channel-token.capnp.

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
    #
    # TODO(soon): This design is overcomplicated since it requires allocating StreamSinks for every
    #   request, even when not used, and requires a lot of weird promise magic. The newer
    #   ExternalPusher design is simpler, and only incurs overhead when used. Once all of
    #   production has been updated to understand ExternalPusher, then we can flip an autogate to
    #   use it by default. Once that has rolled out globally, we can remove StreamSink.

    startStream @0 (externalIndex :UInt32) -> (stream :Capability);
    # Opens a stream corresponding to the given index in the JsValue's `externals` array. The type
    # of capability returned depends on the type of external. E.g. for `readableStream`, it is a
    # `ByteStream`.
  }

  interface ExternalPusher {
    # This object allows "pushing" external objects to a target isolate, so that they can
    # sublequently be referenced by a `JsValue.External`. This allows implementing externals where
    # the sender might need to send subsequent information to the receiver *before* the receiver
    # has had a chance to call back to request it. For example, when a ReadableStream is sent over
    # RPC, the sender will immediately start sending body bytes without waiting for a round trip.
    #
    # The key to ExternalPusher is that it constructs and returns capabilities pointing at objects
    # living directly in the target isolate's runtime. These capabilities have empty interfaces,
    # but can be passed back to the target in the `External` table of a `JsValue`. Since the
    # capabilities point to objects directly in the recipient's memory space, they can then be
    # unwrapped to obtain the underlying local object, which the recipient then uses to back the
    # external value delivered to the application.
    #
    # Note that externals must be pushed BEFORE the JsValue that uses them is sent, so that they
    # can be unwrapped immediately when deserializing the value.

    pushByteStream @0 (lengthPlusOne :UInt64 = 0) -> (source :InputStream, sink :ByteStream);
    # Creates a readable stream within the remote's memory space. `source` should be placed in a
    # sublequent `External` of type `readableStream`. The caller should write bytes to `sink`.
    #
    # `lengthPlusOne` is the expected length of the stream, plus 1, with zero indicating no
    # expectation. This is used e.g. when the `ReadableStream` was created with `FixedLengthStream`.
    # (The weird "plus one" encoding is used because Cap'n Proto doesn't have a Maybe. Perhaps we
    # can fix this eventually.)

    interface InputStream {
      # No methods. This will be unwrapped by the recipient to obtain the underlying local value.
    }

    pushAbortSignal @1 () -> (signal :AbortSignal, trigger :AbortTrigger);

    interface AbortSignal {
      # No methods. This can be unwrapped by the recipient to obtain a Promise<void> which
      # rejects when the signal is aborted.
    }

    # TODO(soon):
    # - AbortTrigger
    # - Promises
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

interface JsRpcTarget extends(JsValue.ExternalPusher) $Cxx.allowCancellation {
  # Target on which RPC methods may be invoked.
  #
  # This is the backing capnp type for a JsRpcStub, as well as used to represent top-level RPC
  # events.
  #
  # JsRpcTarget must implement `JsValue.ExternalPusher` to allow externals to be pushed to the
  # target in advance of a call that uses them.

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

    resultsStreamHandler :union {
      # We're in the process of switching from `StreamSink` to `ExternalPusher`. A caller will only
      # offer one or the other, and expect the callee to use that. (Initially, callers will still
      # send StreamSink for backwards-compatibility, but once all recipients are able to understand
      # ExternalPusher, we'll flip an autogate to make callers send it.)

      streamSink @4 :JsValue.StreamSink;
      # StreamSink used for ReadableStreams found in the results.

      externalPusher @5 :JsValue.ExternalPusher;
      # ExternalPusher object which will push into the caller's isolate. Use this to push externals
      # that will be included in the results.
    }
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

  tailStreamSession @10 () -> (topLevel :TailStreamTarget, result :EventOutcome) $Cxx.allowCancellation;
  # Opens a streaming tail session. The call does not return until the session is complete.
  #
  # `topLevel` is the top-level tail session target, on which exactly one method call can
  # be made. This call must be made using pipelining since `tailStreamSession()` won't return
  # until after the call completes. result is accessed after the session is complete.

  obsolete5 @5();
  obsolete6 @6();
  obsolete7 @7();
  # Deleted methods, do not reuse these numbers.

  # Other methods might be added to handle other kinds of events, e.g. TCP connections, or maybe
  # even native Cap'n Proto RPC eventually.
}

interface WorkerdBootstrap {
  # Bootstrap interface exposed by workerd when serving Cap'n Proto RPC.

  startEvent @0 (cfBlobJson :Text) -> (dispatcher :EventDispatcher);
  # Start a new event. Exactly one event should be delivered to the returned EventDispatcher.
  #
  # If the event is an HTTP request, `cfBlobJson` optionally carries the JSON-encoded `request.cf`
  # object. The dispatcher will pass it through to the worker via SubrequestMetadata.
}

interface WorkerdDebugPort {
  # Bootstrap interface exposed on the debug RPC port, if one is configured. This exposes access
  # to all services in the process, with the ability for the client to specify arbitrary props, so
  # this interface should be considered privileged, and should probably only be used for testing
  # purposes.
  #
  # This interface is subject to change. It is intended for use by miniflare.

  getEntrypoint @0 (service :Text, entrypoint :Text, props :Frankenvalue)
              -> (entrypoint :WorkerdBootstrap);
  # Get direct access to a stateless entrypoint.

  getActor @1 (service :Text, entrypoint :Text, actorId :Text) -> (actor :WorkerdBootstrap);
  # Get an actor (Durable Object) stub.
  # The actorId should be a hex string for Durable Objects or a plain string for ephemeral actors.
}
