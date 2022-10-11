# Welcome to the Workers Streams implementation.

The path here is dark and murky. Tread carefully.

# Terminology

There are a handful of terms that are used commonly throughout the streams implementation.

## Internal streams vs. Standard streams

The original implementation of the streams API in Workers focused solely on providing a
minimal level of functionality necessary for our own use cases. Specifically, things like
reading data from the body of a request, or writing data out to a response. This initial
implementation relies largely on internal kj-based I/O and is only superficially an
implementation of the WHATWG Streams standard. We call this the ***Internal*** streams
implementation.

The ***Standard*** streams are a new separate implementation that conforms to the WHATWG
streams specification. It relies largely on JavaScript Promises to drive the I/O of the
streams themselves. JavaScript functions provided by user code are used to provide both
the source and destination of data flowing through the streams.

While both of these implementations are exposed using the same standard `ReadableStream`,
`WritableStream`, and `TransformStream` APIs, they are two separate implementations that
operate very differently from one another internally.

## Controllers

Every `ReadableStream`, `WritableStream` has an underlying ***controller*** that provides
the actual underlying implementation of the stream itself.

***Internal*** streams and ***Standard*** streams each have separate ***controller***
implementations.

A `WritableStreamController` provides the implementation for a `WritableStream`.

A `ReadableStreamController` provides the implementation for a `ReadableStream`.

The `WritableStreamController` for ***Internal*** streams is the
`WritableStreamInternalController` class defined in `internal.h`.

The `ReadableStreamController` for ***Internal*** streams is the
`ReadableStreamInternalController` class defined in `internal.h`.

The `WritableStreamController` for ***Standard*** streams is the
`WritableStreamJsController` class defined in `standard.h`.

The `ReadableStreamController` for ***Standard*** streams is the
`ReadableStreamJsController` class defined in `standard.h`.

The details on the behavior of each type of controller will be explained later.

## Byte-oriented vs. Value-oriented

A ***Byte-oriented*** stream is only capable of working with byte data expressed in the
form of JavaScript `TypedArray`s and `ArrayBuffer`s. Arbitrary JavaScript values cannot
be passed through ***Byte-oriented*** streams.

A ***Value-oriented*** stream can handle any JavaScript value, including `undefined` and
`null`. `TypedArray`s and `ArrayBuffer`s can be passed through ***Value-oriented*** streams
but are not interpreted as byte sequences.

***Internal*** streams are exclusively ***Byte-oriented***.

***Standard*** streams can be ***Byte-oriented*** or ***Value-oriented***.

Note that the `ReadableStream` and `WritableStream` APIs do not provide any mechanism to
determine through inspection what kind of data a stream provides.

## BYOB vs. Default Readers

A ***Reader*** consumes the data from a `ReadableStream`. There are two kinds of ***Reader***:
***BYOB Readers*** and ***Default Readers***.

A ***BYOB Reader*** works only with ***Byte-oriented*** streams, and allows the calling code
to pass in an arbitrarily sized destination buffer (in the form of a `TypedArray`) that the
***Byte-oriented*** `ReadableStream` will fill with byte data.

A ***Default Reader*** works with both ***Byte-oriented*** and ***Value-oriented*** streams.
The `ReadableStream` will provide the data in whatever form it produces.

# Anatomy of ReadableStream

Let's consider how a `ReadableStream` works. For now, we won't worry about how the `ReadableStream`
is actually created.

```js
const readableStream = getReadableSomehow();
```

Once we have the `ReadableStream` instance, we have to acquire a `Reader` that can consume it.
Here, we acquire a ***Default Reader***.

```
const reader = readableStream.getReader();
```

The ***Default Reader*** can consume data from both ***Byte-oriented*** and ***Value-oriented***
streams using the `read()` method:

```
const chunk = await reader.read();

console.log(chunk.value); // the data that was read.
console.log(chunk.done);  // true or false if the stream has completed.
```

User code calls `read()` repeatedly until the `chunk.done` is `true` indicating that the stream
has been fully consumed.

The flow here is simple enough at the API level, but what is happening under the covers? That's
where things get quite a bit more complex. It also varies depending on whether it is an
***Internal** stream or a ***Standard*** stream.

Let's start with the explanation of the ***Standard*** stream.

## Data-flow in a Standard ReadableStream

A ***Standard*** `ReadableStream` maintains two internal queues:

* A queue of available data
* A queue of pending reads

When a ***Standard*** `ReadableStream` is created, there are four functions (what the spec
calls "algorithms") that the ***controller*** uses:

* The start algorithm -- A function invoked immediately when the `ReadableStream` is created,
  meant to initialize the `ReadableStream`.
* The pull algorithm -- A function invoked to pull more data into the `ReadableStream`.
* The cancel algorithm -- A function invoked when the `ReadableStream` has been explicitly
  canceled.
* The size algorithm -- A function invoked to determine the size of a chunk of data as part
  of the backpressure mechanism.

When the `ReadableStream` is created, the start algorithm is invoked immediately. Once that
function completes, the `ReadableStream` will determine whether or not it should call the
pull algorithm to start the flow of data into the stream. The determination of whether to
call the pull algorithm or not is based entirely on the "highwater mark" of the stream. The
highwater mark is the maximum amount of data (calculated using the size algorithm) that should
be held by the queue of available data at any given time. If the current queue size is less
than the highwater mark, the pull algorithm will be called.

The pull algorithm pushes data into the ReadableStream (and yes, the irony of that statement
is not lost).

* If there are no pending reads when the pull algorithm pushes data in, or if the queue already
  contains data, then the new data into the stream is added to the queue.
* If there is at least one pending read when the data is pushed into the stream the pending read
  is immediately fulfilled with that data. If that pending read does not consume all of the data,
  the excess is pushed into the queue.

For the most part, there should not be pending reads and data pending in the queue at the same
time but there is a bit of temporary overlap allowed in here. Specifically, assuming we have
multiple pending reads stacked up, and the controller enqueues more data than what the first pending
read can handle, on the initial enqueue, the first pending read is fulfilled and the remaining data
is pushed into the queue. After, the remaining pending reads are drained, consuming as much of the
queued data as possible. Once drained, there should be no further pending reads even if data
remains in the queue.

```
        +----------------+
        | pull algorithm | <------------------------------------------+
        +----------------+                                            |
                |                                                     |
                ⊽                                                     |
        +---------------+                                             |
        | enqueue(data) |                                             |
        +---------------+                                             |
                |                                                     |
                ⊽                                                     |
      +--------------------+       +-------------------+              |
      | has a pending read | ----> | has data in queue |              |
      +--------------------+  yes  +-------------------+              |
                |                     |      no|                      |
              no|                     |        ⊽                      |
                ⊽                     |  +----------------------+     |
       +-------------------+    yes   |  | fulfill pending read |     |
       | add data to queue | <--------+  +----------------------+     |
       +-------------------+                    |                     |
                       |                        |                     |
                       |                        ⊽                     |
                       |    +-----------------------------+     no    |
                       +--> | is queue at highwater mark? | -----------
                            +-----------------------------+
                                      yes |
                                          |
                                          ⊽
                                        (done)

```

When the `reader.read()` method is called:

* If there is data already in the queue that can fulfill the read, the read is fulfilled
  immediately from the queue. If removing that data from the queue lowers the current queue
  size below the highwater mark, the stream will call the pull algorithm to pull more data
  into the queue.

* If there is no data in the queue that can fulfill the read, the read request is added to
  the pending read queue, and the queue size is below the highwater mark (which can be 0),
  the stream will call the pull algorithm to pull more data into the queue.

```
    +---------------+
    | reader.read() |
    +---------------+
           |
           ⊽
   +-----------------+       +------------------+       +---------------------+
   | queue has data? | ----> | add pending read | ----> | call pull algorithm |
   +-----------------+  no   +------------------+       +---------------------+
           |
       yes |
           ⊽
    +--------------+
    | fulfill read |
    +--------------+
           |
           |
           ⊽
         (done)
```

It is important to understand that with a ***Standard*** stream, the stream implementation
can continually push data into the internal queue even if there are no active readers and
even if backpressure has already been indicated. This data will continue to build up in the
internal queue, held in memory until it is read.

It is also important to note that it is easily possible to push more data into the queue
than a current attached reader can handle. Any excess data is held in the queue until
subsequent reads consume it.

Another important detail is that it is possible for code to use the controller to enqueue
data outside the lifetime of a `pull()` algorithm's promise. Specifically, once a reference
to the controller is obtained, user code can interact with the controller any time it wishes
completely independent of any of the underlying source algorithms.

### What about tees?

The `ReadableStream` API has a method `tee()` that will split the flow of data from the
`ReadableStream` into two separate `ReadableStream` instances.

In the standard definition of the `ReadableStream` API, the `tee()` method creates two
separate `ReadableStream` instances (called "branches") that share a single `Reader` that
consumes the data from the original `ReadableStream` (let's call it the "trunk"). When one
of the two branches uses the shared `Reader` to pull data from the trunk, that data is
used to fulfill the read request from the pulling branch, and a copy of the data is pushed
into a queue in the other branch. That copied data accumulates in memory until something
starts reading from it.

This spec defined behavior presents a problem for us in that it is possible for one branch
to consume data at a far greater pace than the other, causing the slower branch to accumulate
data in memory without any backpressure controls.

In our implementation, we have modified the `tee()` method implementation to avoid this
issue.

Each branch maintains it's own data buffer. But instead of those buffers containing a
copy of the data, they contain a collection of refcounted references to the data. The
backpressure signaling to the trunk is based on the branch wait the most unconsumed data
in its buffer.

```
   +----------------+
   | pull algorithm |
   +----------------+
           |
           ⊽          ..........................................................
   +---------------+  .   +---------------------+      +-------------------+
   | enqueue(data) | ---> | push data to branch | ---> | has pending read? |
   +---------------+  .   +---------------------+      +-------------------+
               |      .                                no |    yes |
               |      .       +-------------------+       |  +--------------+
               |      .       | add data to queue | <-----+  | fulfill read |
               |      .       +-------------------+          +--------------+
               |      ............................................................
               |      .   +---------------------+      +-------------------+
               +--------> | push data to branch | ---> | has pending read? |
                      .   +---------------------+      +-------------------+
                      .                                no |    yes |
                      .       +-------------------+       |  +--------------+
                      .       | add data to queue | <-----+  | fulfill read |
                      .       +-------------------+          +--------------+
                      ............................................................

```

Unfortunately, with this model, we cannot completely avoid the possibility of one branch
reading much slower than the other but we do prevent the memory pileup that would otherwise
occur *so long as the underlying source of the `ReadableStream` is paying proper attention to
the backpressure signaling mechanisms*.

## Data-flow in an Internal ReadableStream

For ***Internal*** streams the implementation is quite different and it is important to
understand some of the characteristics of ***Standard*** streams first.

First, a ***Standard*** stream maintains a queue of pending reads. At any given time there
can be any number of pending reads in the queue -- specifically, calling code can keep
calling read() over and over. An ***Internal*** stream, however, only allows for a single
pending read at a time.

```js
const readable = new ReadableStream(); // This is always a standard stream
const reader = readable.getReader();
reader.read();
reader.read();  // A second read() works! And is just queued as a pending read.

const readable = request.body;  // This is always an internal stream
const reader = readable.getReader();
reader.read();
reader.read();  // Fails with an error!
```

An ***Internal*** stream is backed by an object called a `ReadableStreamSource`, which is
a kj heap object that is a thin wrapper around the `kj::AsyncInputStream` API. Calling `read()`
on an ***Internal*** stream leads directly to a `tryRead()` on the `ReadableStreamSource`,
which returns a `kj::Promise` for a chunk of data from the underlying stream. Only a single
read is permitted to be in flight at any given time, and the promise will be pending until
the minimum amount of data asked for is requested. In an ***Internal*** stream, there is no
concept of a pull algorithm. There is also no concept of an internal data queue.

```
    +---------------+
    | reader.read() |
    +---------------+
            |
            ⊽
  +-------------------+      +-----------------------------------+
  | has pending read? | ---> | tryRead() on ReadableStreamSource |
  +-------------------+  no  +-----------------------------------+
        yes |
            ⊽
        +-------+
        | error |
        +-------+
```

Unlike a ***Standard*** stream, data only flows through an ***Internal*** stream only if
there is an active reader, and each read will never exceed a maximum number of bytes
specified in the read call.

# Anatomy of a WritableStream

## Data flow of a Standard WritableStream

A ***Standard*** `WritableStream` maintains a queue of pending writes and uses a handful
of functions (again, called "algorithms") to implement the flow of data:

* The start algorithm is invoked with the `WritableStream` is created to prepare the
  stream to receive data.
* The write algorithm is called for every chunk of data written to the stream.
* The abort algorithm is called when the stream is abruptly terminated.
* The close algorithm is called after the last chunk of data has been written.
* The size algorithm is called for every chunk to help calculate the highwater mark.

```js
const writable = getWritableSomehow();
const writer = writable.getWriter();

await writer.write("chunk of data");
await writer.write("another chunk of data");
```

The write algorithm is asynchronous. For every `write(data)` called on the `Writer`,
the write algorithm is invoked. When the write algorithm completes, the `writer.write()`
promise will be resolved. The write promise is the primary mechanism for backpressure
in the `WritableStream`. A highwater mark is calculated but it is up to the code using
the `Writer` to check it. If the highwater mark is less than or equal to zero, then it
should not call `write`. If the write algorithm is overloaded, then it should not resolve
the write algorithm promise until it is ready to handle more data. However, a write
algorithm should always expect to be called no matter what the highwater mark value is.

Backpressure on the writer is signaled in two ways:

* Using the `writer.desiredSize` property, which communicates the amount of data that
  may be written before the internal queue is full.
* Using the `writer.ready` property, which returns a promise that will be resolved
  when backpressure is cleared. The `writer.ready` promise is replaced with a new
  promise every time backpressure is signaled on the stream.

## Data flow of an Internal WritableStream

An ***Internal*** `WritableStream` is backed by a kj heap object called a
`WritableStreamSink`, which is a thin layer on top of the `kj::AsyncOutputStream` API.
Every time `write()` is called on the `writer()`, that is passed down to the `write()`
API on the underlying `WritableStreamSink`. The details from there are dependent on
the specific implementation of the `WritableStreamSink`.

# Internal vs. Standard TransformStreams

A `TransformStream` is a connected set consisting of a `ReadableStream` and a `WritableStream`.

Whatever data is written to the `WritableStream` (the "writable" side) can be read back out
on the `ReadableStream` (the "readable" side), potentially transformed into some other value.

## Internal TransformStream

The original implementation of the `TransformStream` API acts as a simple "identity transform" --
that is, the data written to the writable side is passed through, unmodified, to the readable
side (that is, it is essentially just a simple pipe). It is implemented by creating a single
underlying class that implements both the `ReadableStreamSource` and `WritableStreamSink` APIs
and passing that single shared instance to both the `ReadableStream` and `WritableStream` as
the ***controller***.

Unfortunately, this initial implementation is not compliant with the standard specification.
In order to properly conform to the standard, a compatibility flag is introduced to change
the behavior of the `new TransformStream()` constructor, and the old implementation of
the ***Internal*** `TransformStream` has been moved to a new `IdentityTransformStream` class.
When the compatibility flag is disabled, `new TransformStream()` is an alias for
`new IdentityTransformStream()`.

```js
const { readable, writable } = new IdentityTransformStream();
const enc = new TextEncoder();

const writer = writable.getWriter();
const reader = readable.getReader();

// The write promise will not resolve until read() is called!
await Promise.all([
  writer.write(enc.encode('hello')),
  reader.read(),
]);
```

The `IdentityTransformStream` acts as a simple state machine that permits only a single
write or single read at a time.

Initially, the state is `idle`.

If a `write()` occurs and:

* The state is `idle`, the state becomes `write pending`, the written data is held, and a pending
  write promise is returned.
* If the state is `read pending`, the read promise is fulfilled with the data and the state returns
  to `idle`, and an immediately fulfilled write promise is returned.

If a `read()` occurs and:

* The state is `idle`, the state becomes `read pending`, and a pending read promise is returned.
* The state is `write pending`, the read is immediately fulfilled with the pending write data.

This model permits only a single in flight read or write at a time. A `write()` promise will not
be fulfilled until there is a corresponding `read()`. A `read()` promise will not be fulfilled
until there is a corresponding `write()`.

Importantly, the old ***Internal*** `TransformStream` implementation only supports byte-data in
the form of `ArrayBuffer` or `TypedArray` instances.

## Standard TransformStream

A ***Standard*** TransformStream is a lot more complicated but also a lot more powerful.

The `TransformStream` uses a number of algorithms:

* A start algorithm that is invoked when the `TransformStream` is created to initialize the
  transformation.
* A transform algorithm that receives a chunk of data, performs some operation to modify
  it, then enqueues the result for reading.
* A flush algorithm that completes the transformation process.

On the writable side of the `TransformStream`, every call to `write()` passes the data
into the transform algorithm, which in turn enqueues the modified data in the readable
side.

The two sides operate exactly the same as any other ***Standard*** `ReadableStream` and
`WritableStream`, allowing for multiple concurrently pending reads and writes, and
maintaining internal queues with backpressure. A read promise is not blocked if there is
data pending in the queue, and writes promises is not blocked on reads unless backpressure
signals that the internal queue is full.

Any JavaScript value can be written to, and read from, a ***Standard*** `TransformStream`.

```js
const { writable, readable } = new TransformStream({
  transform(chunk, controller) {
    controller.enqueue(`${chunk}!`.toUpperCase());
  }
});

const writer = writable.getWriter();
const reader = readable.getReader();

// The write promise does not wait for a read.
await writer.write('hello');

await reader.read();  // Outputs 'HELLO!'
```

# Lots of Moving Parts

The streams implementation is complicated. This much should be painfully obvious so far.
Not only is the Streams Standard quite complicated, we're balancing two different
implementations of that specification that have different capabilities, there are two
different kinds of streams (***Byte-oriented*** and ***Value-oriented***), two different
memory heaps (JavaScript and kj), two different async models (JavaScript promises and
kj promises), and a constraint that says we can't break backwards compatibility without
have feature flags in place to switch between old and new functionality.

The Streams standard is defined in terms of JavaScript promises. Specifically, the
`read()` and `write()` operations return JavaScript promises -- the read() promise
fulfilled when data is available to be read, and the write() promise fulfilled when
the data written has been handled by whatever underlying sink is meant to handle it.

For ***Internal*** streams, these are driven by the `ReadableStreamSource` and
`WritableStreamSink` APIs, both of which exist in the kj heap space, and both of
which are built on top of the kj asynchronous i/o and kj::Promise model. Importantly,
the flow of data through an ***Internal*** stream is always byte-oriented and will,
most often, be handled outside of the JavaScript isolate lock (in other words, writes
and reads are actually fulfilled when there is no JavaScript running).

For ***Standard*** streams, the entire flow of data always occurs within the JavaScript
isolate lock, and the entire flow is defined in terms of JavaScript promises. All data
is held within the JavaScript heap.

When using these different types of streams *together*, things become complicated pretty
quicky.

## pipeTo/pipeThrough

"Piping" is setting up the flow of data from a `ReadableStream` to a `WritableStream`.

How this is accomplished depends entirely on what kind of readable or writable we're
working with. In our implementation, we allow the destination writable to determine
how exactly the pipe should be implemented.

```
         +---------------------------+
         | readable.pipeTo(writable) |
         +---------------------------+
                       |
                       |
                       ⊽
   +------------------------------------------+
   | writableController.tryPipeFrom(readable) |
   +------------------------------------------+
                       |
                       |
                       ⊽
           +-----------------------+       +-----------------------+
           | is internal writable? | ----> | is internal readable? |
           +-----------------------+  yes  +-----------------------+
                       |                      no |         yes |
                    no |                         |             |
                       ⊽                    +----+     +---------------+
           +-----------------------+        |          | kj-to-kj pipe |
           | is internal readable? |        |          |      loop     |
           +-----------------------+        |          +---------------+
            no  |           yes |           |
                |               |           |
                ⊽               |           |
         +---------------+      |           ⊽
         | JS-to-JS pipe |      |   +---------------+
         |      loop     |      |   | JS-to-kj pipe |
         +---------------+      |   |     loop      |
                                ⊽   +---------------+
                        +---------------+
                        | kj-to-JS pipe |
                        |     loop      |
                        +---------------+
```

### Piping data from an Internal ReadableStream to an Internal WritableStream (kj-to-kj pipe loop)

This is an easy case. Here, kj implements optimized mechanisms for piping data from a
`kj::AsyncInputStream` to a `kj::AsyncOutputStream`. The flow of data is driven outside
of the JavaScript isolate lock and entirely by the kj event loop. The data flow is
highly optimized and does not require any JavaScript to be run to drive things forward.

### Piping data between Internal and Standard streams

When piping data from either an Internal Readable to a Standard Writable, or from a
Standard Readable to an Internal Writable, the implementation is necessarily not as
optimized. First, the entire data flow has to occur within the JavaScript isolate
lock. Second, we have to rely on bridging JavaScript and kj Promises. Third, we have
to limit the type of data handled to byte data (***Standard*** streams, remember, can
be either ***Byte-oriented*** or ***Value-oriented*** and we can't know for sure
because the streams API provides no way of inspecting it).

#### JavaScript-to-JavaScript pipe loop

When piping data from a Standard Readable to a Standard Writable, every read and every
write is a JavaScript promise. Driving the pipe is essentially a process of chaining
the JavaScript promises together.

An overly-simplified equivalent JavaScript implementation would look something like:

```
async function pipe(reader, writer) {
  for await (const chunk of reader) {
    await writer.write(chunk);
  }
}
```

The entire pipe process is implemented within the JavaScript isolate lock and using v8
promises entirely.

#### JavaScript-to-kj pipe loop

When piping data from a Standard Readable to an Internal Writable the flow becomes a bit
more complicated because of the need to depend on kj async i/o. Reads are based on the
JavaScript promise, but then writes require dropping out to the kj promises and allowing
the kj event loop to advance to complete the write.

#### kj-to-JavaScript pipe loop

Similarly, piping from an Internal Readable to a Standard Writable requires bridging the
kj event loop to the JavaScript promises. Reads are based on async kj i/o and writes
occur as a JavaScript flow.

## new Response(standardReadable)

Passing a ***Standard*** `ReadableStream` off to APIs like `new Response()` is fairly
complicated because:

* The `new Response()` API (and others like it) were implemented originally with only
  ***Internal*** streams in mind -- and therefore implement everything in terms of the
  kj async i/o mechanisms and kj::Promises.
* We have to continue supporting both ***Internal*** and ***Standard*** streams with these
  APIs without introducing any non-backwards compatible changes when using ***Internal***
  streams.

To make it work, ***Standard*** `ReadableStreams` include the ability to be consumed via
the internal `ReadableStreamSource` API (the same underlying API that ***Internal*** streams
use). When the `pumpTo()` method on the `ReadableStreamSource` acquired from a ***Standard***
stream is called, an isolate lock will be acquired and a promise loop will be kicked off.
In each iteration of the loop we first perform a read followed by a write if the read does
not end the stream. The promise loop ends either when all of the data has been consumed or
the stream errors, whichever comes first.

