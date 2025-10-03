#include "readable-source.h"

#include "writable-sink.h"

#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/stream-utils.h>
#include <workerd/util/string-buffer.h>
#include <workerd/util/strong-bool.h>

#include <kj/async-io.h>
#include <kj/compat/brotli.h>
#include <kj/compat/gzip.h>
#include <kj/one-of.h>

namespace workerd::api::streams {

namespace {
// Used to consume and collect all data from a ReadableStreamSource up to a specified
// limit. Throws if the limit is exceeded before EOF.
class AllReader final {
 public:
  explicit AllReader(ReadableStreamSource& input, size_t limit): input(input), limit(limit) {
    JSG_REQUIRE(limit > 0, TypeError, "Memory limit exceeded before EOF.");
    KJ_IF_SOME(length, input.tryGetLength(rpc::StreamEncoding::IDENTITY)) {
      // Oh hey, we might be able to bail early.
      JSG_REQUIRE(length <= limit, TypeError, "Memory limit would be exceeded before EOF.");
    }
  }
  KJ_DISALLOW_COPY_AND_MOVE(AllReader);

  kj::Promise<kj::Array<const kj::byte>> readAllBytes() {
    co_return co_await read<kj::byte>();
  }

  kj::Promise<kj::String> readAllText() {
    co_return kj::String(co_await read<char>(ReadOption::NULL_TERMINATE));
  }

 private:
  ReadableStreamSource& input;
  size_t limit;

  enum class ReadOption {
    NONE,
    NULL_TERMINATE,
  };

  template <typename T>
  kj::Promise<kj::Array<T>> read(ReadOption option = ReadOption::NONE) {
    // Read in chunks and accumulate them. Use an exponential growth strategy
    // to determine chunk sizes to minimize the number of iterations and
    // allocations on large streams.
    kj::Vector<kj::Array<T>> parts;
    size_t runningTotal = 0;
    // TODO(later): Make these configurable someday?
    static constexpr size_t MIN_BUFFER_CHUNK = 1024;
    static constexpr size_t DEFAULT_BUFFER_CHUNK = 4096;
    // TODO(later): Consider increasing MAX_BUFFER_CHUNK, maybe up to 1 MB?
    static constexpr size_t MAX_BUFFER_CHUNK = DEFAULT_BUFFER_CHUNK * 4;

    // If we know in advance how much data we'll be reading, then we can attempt to optimize the
    // loop here by setting the value specifically so we are only allocating at most twice. But,
    // to be safe, let's enforce an upper bound on each allocation even if we do know the total.
    kj::Maybe<size_t> maybeLength = input.tryGetLength(rpc::StreamEncoding::IDENTITY);

    size_t amountToRead;
    KJ_IF_SOME(length, maybeLength) {
      if (length <= MAX_BUFFER_CHUNK) {
        amountToRead = kj::min(limit, length);
      } else {
        amountToRead = DEFAULT_BUFFER_CHUNK;
      }
    } else {
      amountToRead = MIN_BUFFER_CHUNK;
    }

    if (amountToRead != 0) {
      while (true) {
        auto bytes = kj::heapArray<T>(amountToRead);
        size_t amount = co_await input.read(bytes.asBytes(), bytes.size());
        KJ_DASSERT(amount <= bytes.size());
        runningTotal += amount;
        JSG_REQUIRE(runningTotal <= limit, TypeError, "Memory limit exceeded before EOF.");

        if (amount == bytes.size()) {
          parts.add(kj::mv(bytes));
          // Adjust the next allocation size -- double it up to a maximum
          amountToRead = kj::min(amountToRead * 2, kj::min(MAX_BUFFER_CHUNK, limit - runningTotal));
        } else {
          if (amount > 0) {
            parts.add(bytes.first(amount).attach(kj::mv(bytes)));
          }
          break;
        }
      }
    }

    if (option == ReadOption::NULL_TERMINATE) {
      auto out = kj::heapArray<T>(runningTotal + 1);
      out[runningTotal] = '\0';
      copyInto<T>(out, parts);
      co_return kj::mv(out);
    }

    // As an optimization, if there's only a single part in the list, we can avoid
    // further copies.
    if (parts.size() == 1) {
      co_return kj::mv(parts[0]);
    }

    auto out = kj::heapArray<T>(runningTotal);
    copyInto<T>(out, parts);
    co_return kj::mv(out);
  }

  template <typename T>
  void copyInto(kj::ArrayPtr<T> out, kj::ArrayPtr<kj::Array<T>> in) {
    for (auto& part: in) {
      KJ_DASSERT(part.size() <= out.size());
      out.first(part.size()).copyFrom(part);
      out = out.slice(part.size());
    }
  }
};

// An AsyncInputStream wrapper that translates tee-related kj::Exceptions from read
// operations into jsg::Exceptions.
// TODO(later): We might be able to get rid of this and use a KJ exception detail instead.
class TeeErrorAdapter final: public kj::AsyncInputStream {
 public:
  static kj::Own<kj::AsyncInputStream> wrap(kj::Own<kj::AsyncInputStream> inner) {
    // We make a best effort to avoid double-wrapping.
    if (dynamic_cast<TeeErrorAdapter*>(inner.get()) == nullptr) {
      return kj::heap<TeeErrorAdapter>(kj::mv(inner));
    } else {
      return kj::mv(inner);
    }
  }

  explicit TeeErrorAdapter(kj::Own<AsyncInputStream> inner): inner(kj::mv(inner)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return translateErrors([&] { return inner->tryRead(buffer, minBytes, maxBytes); });
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return inner->tryGetLength();
  };

  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    return translateErrors([&] { return inner->pumpTo(output, amount); });
  }

  kj::Maybe<kj::Own<kj::AsyncInputStream>> tryTee(uint64_t limit) override {
    return inner->tryTee(limit);
  }

 private:
  kj::Own<AsyncInputStream> inner;

  template <typename Func>
  static auto translateErrors(Func&& f) -> decltype(kj::fwd<Func>(f)()) {
    try {
      co_return co_await f();
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_IF_SOME(translated,
          translateKjException(exception,
              {
                {"tee buffer size limit exceeded"_kj,
                  "ReadableStream.tee() buffer limit exceeded. This error usually occurs "
                  "when a Request or Response with a large body is cloned, then only one "
                  "of the clones is read, forcing the Workers runtime to buffer the entire "
                  "body in memory. To fix this issue, remove unnecessary calls to "
                  "Request/Response.clone() and ReadableStream.tee(), and always read "
                  "clones/tees in parallel."_kj},
              })) {
        kj::throwFatalException(kj::mv(translated));
      } else {
        kj::throwFatalException(kj::mv(exception));
      }
    }
  }
};

// A kj::AsyncInputStream implementation that delegates to a provided function
// to produce data on each read.
class InputStreamFromProducer final: public kj::AsyncInputStream {
 public:
  using Producer = kj::Function<kj::Promise<size_t>(kj::ArrayPtr<kj::byte>, size_t)>;
  InputStreamFromProducer(Producer producer, kj::Maybe<uint64_t> expectedLength)
      : producer(kj::mv(producer)),
        expectedLength(expectedLength) {}

  KJ_DISALLOW_COPY_AND_MOVE(InputStreamFromProducer);

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    KJ_IF_SOME(p, producer) {
      // If there is an expected length, we won't try to read more than whatever is remaining.
      maxBytes = kj::min(maxBytes, expectedLength.orDefault(maxBytes));
      minBytes = kj::min(minBytes, maxBytes);
      auto amount = co_await p(kj::arrayPtr(static_cast<kj::byte*>(buffer), maxBytes), minBytes);
      KJ_IF_SOME(length, expectedLength) {
        KJ_DASSERT(amount <= length, "Producer produced more data than expected.");
        length -= amount;
      }
      if (amount < minBytes) {
        // The producer is indicating that we're done. Drop the producer.
        // If the producer did not producer as much data as we expected, that's an error.
        KJ_IF_SOME(length, expectedLength) {
          KJ_REQUIRE(length == 0, "jsg.Error: Producer ended stream early.");
        }
        producer = kj::none;
      }
      co_return amount;
    } else {
      co_return 0;  // EOF
    }
  }

  // Returns the expected number of bytes remaining to be read, if known.
  kj::Maybe<uint64_t> tryGetLength() override {
    return expectedLength;
  }

 private:
  kj::Maybe<kj::Function<kj::Promise<size_t>(kj::ArrayPtr<kj::byte>, size_t)>> producer;
  kj::Maybe<uint64_t> expectedLength;
};

// A base class for ReadableStreamSource implementations that provides default
// implementations of some methods.
class ReadableStreamSourceImpl: public ReadableStreamSource {
 public:
  ReadableStreamSourceImpl(kj::Own<kj::AsyncInputStream> input,
      rpc::StreamEncoding encoding = rpc::StreamEncoding::IDENTITY)
      : state(kj::mv(input)),
        encoding(encoding) {}
  ReadableStreamSourceImpl(kj::Exception reason)
      : state(kj::mv(reason)),
        encoding(rpc::StreamEncoding::IDENTITY) {}
  ReadableStreamSourceImpl(): state(Closed()), encoding(rpc::StreamEncoding::IDENTITY) {}
  KJ_DISALLOW_COPY_AND_MOVE(ReadableStreamSourceImpl);
  virtual ~ReadableStreamSourceImpl() noexcept(false) {
    canceler.cancel(KJ_EXCEPTION(DISCONNECTED, "stream was dropped"));
  }

  kj::Promise<size_t> readInner(
      kj::Own<kj::AsyncInputStream>& inner, kj::ArrayPtr<kj::byte> buffer, size_t minBytes = 1) {
    try {
      auto& stream = setStream(ensureIdentityEncoding(kj::mv(inner)));
      minBytes = kj::max(minBytes, 1u);
      auto amount = co_await readImpl(stream, buffer, minBytes);
      if (amount < minBytes) {
        setClosed();
      }
      co_return amount;
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      setErrored(kj::cp(exception));
      kj::throwFatalException(kj::mv(exception));
    }
  }

  kj::Promise<size_t> read(kj::ArrayPtr<kj::byte> buffer, size_t minBytes = 1) override {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(errored, kj::Exception) {
        kj::throwFatalException(kj::cp(errored));
      }
      KJ_CASE_ONEOF(_, Closed) {
        co_return 0;
      }
      KJ_CASE_ONEOF(inner, kj::Own<kj::AsyncInputStream>) {
        KJ_REQUIRE(canceler.isEmpty(), "jsg.Error: Stream is already being read");
        co_return co_await canceler.wrap(readInner(inner, buffer, minBytes));
        // If the source is dropped while a read is in progress, the canceler will
        // trigger and abort the read. In such cases, we don't want to wrap this
        // await in a try catch because it isn't safe to continue using the stream
        // as it may no longer exist.
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Promise<DeferredProxy<void>> pumpTo(
      WritableStreamSink& output, EndAfterPump end = EndAfterPump::YES) override {
    // By default, we assume the pump is eligible for deferred proxying.
    KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING;

    if (!canceler.isEmpty()) {
      kj::throwFatalException(KJ_EXCEPTION(FAILED, "jsg.Error: Stream is already being read"));
    }

    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(inner, kj::Own<kj::AsyncInputStream>) {
        // Ownership of the underlying inner stream is transferred to the pump operation,
        // where it will be either fully consumed or errored out. In either case, this
        // ReadableSource becomes closed and no longer usable once pumpTo() is called.
        // Critically... it is important that just because the ReadableSource is closed here
        // does NOT mean that the underlying stream has been fully consumed.
        auto stream = kj::mv(inner);
        setClosed();

        if (output.getEncoding() != getEncoding()) {
          // The target encoding is different from our current encoding.
          // Let's ensure that our side is in identity encoding. The destination stream will
          // take care of itself.
          stream = ensureIdentityEncoding(kj::mv(stream));
        } else {
          // Since the encodings match, we can tell the output stream that it doesn't need to
          // do any of the encoding work since we'll be providing data in the expected encoding.
          KJ_ASSERT(getEncoding() == output.disownEncodingResponsibility());
        }

        // Note that because we are transferring ownership of the stream to the pump operation,
        // and the pump itself should not rely on the ReadableStreamSource for any state, it is
        // safe to drop the ReadableStreamSource once the pump operation begins.
        co_return co_await pumpImpl(kj::mv(stream), output, end);
      }
      KJ_CASE_ONEOF(closed, Closed) {
        if (end) {
          co_await output.end();
        }
        co_return;
      }
      KJ_CASE_ONEOF(errored, kj::Exception) {
        output.abort(kj::cp(errored));
        throwFatalException(kj::mv(errored));
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Maybe<size_t> tryGetLength(rpc::StreamEncoding encoding) override {
    if (encoding == rpc::StreamEncoding::IDENTITY) {
      KJ_IF_SOME(active, state.tryGet<kj::Own<kj::AsyncInputStream>>()) {
        return active->tryGetLength();
      }
    }
    return kj::none;
  }

  kj::Promise<kj::Array<const kj::byte>> readAllBytes(size_t limit) override {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(_, Closed) {
        co_return kj::Array<const kj::byte>();
      }
      KJ_CASE_ONEOF(errored, kj::Exception) {
        kj::throwFatalException(kj::cp(errored));
      }
      KJ_CASE_ONEOF(_, kj::Own<kj::AsyncInputStream>) {
        AllReader reader(*this, limit);
        co_return co_await reader.readAllBytes();
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Promise<kj::String> readAllText(size_t limit) override {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(_, Closed) {
        co_return kj::String();
      }
      KJ_CASE_ONEOF(errored, kj::Exception) {
        kj::throwFatalException(kj::cp(errored));
      }
      KJ_CASE_ONEOF(_, kj::Own<kj::AsyncInputStream>) {
        AllReader reader(*this, limit);
        co_return co_await reader.readAllText();
      }
    }
    KJ_UNREACHABLE;
  }

  void cancel(kj::Exception reason) override {
    canceler.cancel(kj::cp(reason));
    setErrored(kj::mv(reason));
  }

  Tee tee(size_t limit) override {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(errored, kj::Exception) {
        return Tee{
          .branch1 = newErroredReadableStreamSource(kj::cp(errored)),
          .branch2 = newErroredReadableStreamSource(kj::cp(errored)),
        };
      }
      KJ_CASE_ONEOF(_, Closed) {
        return Tee{
          .branch1 = newClosedReadableStreamSource(),
          .branch2 = newClosedReadableStreamSource(),
        };
      }
      KJ_CASE_ONEOF(stream, kj::Own<kj::AsyncInputStream>) {
        KJ_DEFER(state.init<Closed>());
        KJ_IF_SOME(tee, tryTee(limit)) {
          return kj::mv(tee);
        }

        auto tee = kj::newTee(kj::mv(stream), limit);
        return Tee{
          .branch1 = newReadableStreamSource(wrapTeeBranch(kj::mv(tee.branches[0]))),
          .branch2 = newReadableStreamSource(wrapTeeBranch(kj::mv(tee.branches[1]))),
        };
      }
    }
    KJ_UNREACHABLE;
  }

  rpc::StreamEncoding getEncoding() override {
    return encoding;
  }

 protected:
  struct Closed {};

  // Implementations really should override this to provide encoding support!
  virtual kj::Own<kj::AsyncInputStream> ensureIdentityEncoding(
      kj::Own<kj::AsyncInputStream>&& inner) {
    // By default, we always use identity encoding so nothing to do here.
    // It is up to subclasses to override this if they support other encodings.
    KJ_DASSERT(encoding == rpc::StreamEncoding::IDENTITY);
    return kj::mv(inner);
  }

  // Implementations should override to provide an alternative tee implementation.
  // This will only be called when the state is known to be not closed or errored.
  virtual kj::Maybe<Tee> tryTee(size_t limit) {
    return kj::none;
  }

  kj::OneOf<kj::Own<kj::AsyncInputStream>, Closed, kj::Exception>& getState() {
    return state;
  }

  void setClosed() {
    state.init<Closed>();
  }

  void setErrored(kj::Exception reason) {
    state = kj::mv(reason);
  }

  kj::AsyncInputStream& setStream(kj::Own<kj::AsyncInputStream> stream) {
    auto& inner = *stream;
    state = kj::mv(stream);
    return inner;
  }

  void setEncoding(rpc::StreamEncoding newEncoding) {
    encoding = newEncoding;
  }

 private:
  kj::OneOf<kj::Own<kj::AsyncInputStream>, Closed, kj::Exception> state;
  rpc::StreamEncoding encoding;
  kj::Canceler canceler;

  struct ReadOption {
    bool identityEncoding;
  };

  // The default pumpTo() implementation which initiates a loop
  // that reads a chunk from the input stream and writes it to the output
  // stream until EOF is reached.
  // The pump is canceled by dropping the returned promise.
  static kj::Promise<void> pumpImpl(
      kj::Own<kj::AsyncInputStream> stream, WritableStreamSink& output, EndAfterPump end) {
    // These are fairly arbitrary but reasonable buffer size choices.
    static constexpr size_t DEFAULT_BUFFER_SIZE = 16384;
    static constexpr size_t MIN_BUFFER_SIZE = 1024;
    static constexpr size_t MED_BUFFER_SIZE = 65536;
    static constexpr size_t MAX_BUFFER_SIZE = 131072;
    static constexpr size_t SMALL_THRESHOLD = 4096;
    static constexpr size_t MEDIUM_THRESHOLD = 1048576;

    // Determine optimal buffer size based on stream length. If the stream does
    // not report a length, use the default. The logic here is simple: use larger
    // buffer sizes for larger streams to reduce the number of read/write iterations.
    // and smaller buffer sizes for smaller streams to reduce memory usage.
    // If the size is unknown, we defer to a reasonable default.
    size_t bufferSize = DEFAULT_BUFFER_SIZE;
    kj::Maybe<uint64_t> maybeRemaining = stream->tryGetLength();
    KJ_IF_SOME(length, maybeRemaining) {
      if (length < SMALL_THRESHOLD) {
        bufferSize = kj::max(MIN_BUFFER_SIZE, length);
      } else if (length > DEFAULT_BUFFER_SIZE && length <= MEDIUM_THRESHOLD) {
        bufferSize = MED_BUFFER_SIZE;
      } else if (length > MEDIUM_THRESHOLD) {
        bufferSize = MAX_BUFFER_SIZE;
      }
    }

    // We use a double-buffering/pipelining strategy here to try to keep both the read
    // and write operations busy in parallel. While one buffer is being written to the
    // output, the other buffer is being filled with data from the input stream. It does
    // mean that we use a bit more memory in the process but should improve throughput on
    // high-latency streams.
    int current = 0;
    KJ_STACK_ARRAY(kj::byte, backing, bufferSize * 2, 4 * MIN_BUFFER_SIZE, 4 * MIN_BUFFER_SIZE);
    kj::ArrayPtr<kj::byte> buffer[] = {
      backing.first(bufferSize),
      backing.slice(bufferSize),
    };

    // We will use an adaptive minBytes value to try to optimize read sizes based on
    // observed stream behavior. We start with a minBytes set to half the buffer size.
    // As the stream is read, we will adjust minBytes up or down depending on whether
    // the stream is consistently filling the buffer or not.
    size_t minBytes = bufferSize >> 1;

    auto readPromise = readImpl(*stream, buffer[current], minBytes);
    size_t iterationCount = 0;

    while (true) {
      // On each iteration, wait for the read to complete...
      size_t amount = co_await readPromise;
      iterationCount++;

      // If we read less than minBytes, assume EOF.
      if (amount < minBytes) {
        // If any bytes were read...
        if (amount > 0) {
          // Write our final chunk...
          co_await output.write(buffer[current].first(amount));
        }
        // Then break out of the loop.
        break;
      }

      // Set the write buffer to the one we just filled.
      auto writeBuf = buffer[current];

      // Then switch to the other buffer and start the next read.
      current = 1 - current;

      // Before we perform the next read, let's adapt minBytes based on stream behavior
      // we have observed on the previous read.
      if (iterationCount <= 3 || iterationCount % 10 == 0) {
        if (amount == bufferSize) {
          // Stream is filling buffer completely... Use smaller minBytes to
          // increase responsiveness, should produce more reads with less data.
          minBytes = kj::max(bufferSize >> 2, kj::min(DEFAULT_BUFFER_SIZE, bufferSize >> 1));
        } else {
          // Stream is moving slower, increase minBytes to try to get larger chunks.
          minBytes = (bufferSize >> 2) + (bufferSize >> 1);  // 75%
        }
      }

      KJ_IF_SOME(remaining, maybeRemaining) {
        if (amount > remaining) {
          // The stream lied about its length. Ignore further length tracking.
          maybeRemaining = kj::none;
        } else {
          // Otherwise, set minBytes to whatever is expected to remain.
          remaining -= amount;
          maybeRemaining = remaining;
          if (remaining < minBytes && remaining > 0) {
            minBytes = remaining;
          }
        }
      }

      // Start our next read operation.
      readPromise = readImpl(*stream, buffer[current], minBytes);

      // Write out the chunk we just read in parallel with the next read.
      // If the write fails, the exception will propagate and cancel the pump,
      // including the read operation. If the read fails, it will be picked
      // up at the start of the next loop iteration.
      co_await output.write(writeBuf.first(amount));
    }

    if (end) co_await output.end();
  }

  static kj::Promise<size_t> readImpl(
      kj::AsyncInputStream& inner, kj::ArrayPtr<kj::byte> buffer, size_t minBytes) {
    KJ_ASSERT(minBytes <= buffer.size());
    try {
      // The read() method on AsyncInputStream will throw an exception on short reads.
      co_return co_await inner.tryRead(buffer.begin(), minBytes, buffer.size());
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
        // Treat disconnects as EOF.
        co_return 0;
      }
      kj::throwFatalException(kj::mv(exception));
    }
  }
};

// A ReadableStreamSource wrapper that prevents deferred proxying. This is useful
// when you expect that the IoContext will need to remain live for the duration
// of the operations on the stream.
class NoDeferredProxySource final: public ReadableStreamSourceWrapper {
 public:
  NoDeferredProxySource(kj::Own<ReadableStreamSource> inner, IoContext& ioctx)
      : ReadableStreamSourceWrapper(kj::mv(inner)),
        ioctx(ioctx) {}

  kj::Promise<size_t> read(kj::ArrayPtr<kj::byte> buffer, size_t minBytes = 1) override {
    auto pending = ioctx.registerPendingEvent();
    co_return co_await getInner().read(buffer, minBytes);
  }

  kj::Promise<DeferredProxy<void>> pumpTo(
      WritableStreamSink& output, EndAfterPump end = EndAfterPump::YES) override {
    auto pending = ioctx.registerPendingEvent();
    return addNoopDeferredProxy(
        ioctx.waitForDeferredProxy(getInner().pumpTo(output, end)).attach(kj::mv(pending)));
  }

  Tee tee(size_t limit) override {
    auto tee = getInner().tee(limit);
    return Tee{
      .branch1 = kj::heap<NoDeferredProxySource>(kj::mv(tee.branch1), ioctx),
      .branch2 = kj::heap<NoDeferredProxySource>(kj::mv(tee.branch2), ioctx),
    };
  }

 private:
  IoContext& ioctx;
};

static const WarningAggregator::Key unusedStreamBranchKey;
// A ReadableStreamSource wrapper that emits a warning if it is never read from
// before being destroyed. The warning aggregates multiple instances together and
// prints a single warning message when the associated WarningAggregator is destroyed.
// The message includes a stack trace of where each unused stream was created to
// aid in debugging.
class WarnIfUnusedStream final: public ReadableStreamSourceWrapper {
 public:
  class UnusedStreamWarningContext final: public WarningAggregator::WarningContext {
   public:
    UnusedStreamWarningContext(jsg::Lock& js): exception(jsg::JsRef(js, js.error(""_kjc))) {}

    kj::String toString(jsg::Lock& js) override {
      auto handle = exception.getHandle(js);
      auto obj = KJ_ASSERT_NONNULL(handle.tryCast<jsg::JsObject>());
      obj.set(js, "name"_kjc, js.strIntern("Unused stream created:"_kjc));
      return obj.get(js, "stack"_kjc).toString(js);
    }

   private:
    jsg::JsRef<jsg::JsValue> exception;
  };

  static kj::Own<WarningAggregator> createWarningAggregator(IoContext& context) {
    return kj::atomicRefcounted<WarningAggregator>(
        context, [](jsg::Lock& js, kj::Array<kj::Own<WarningAggregator::WarningContext>> warnings) {
      StringBuffer<1024> message(1024);
      if (warnings.size() > 1) {
        message.append(
            kj::str(warnings.size()), " ReadableStream branches were created but never consumed. ");
      } else {
        message.append("A ReadableStream branch was created but never consumed. ");
      }
      message.append("Such branches can be created, for instance, by calling the tee() "
                     "method on a ReadableStream, or by calling the clone() method on a "
                     "Request or Response object. If a branch is created but never consumed, "
                     "it can force the runtime to buffer the entire body of the stream in "
                     "memory, which may cause the Worker to exceed its memory limit and be "
                     "terminated. To avoid this, ensure that all branches created are consumed.\n");

      if (warnings.size() > 1) {
        for (int n = 0; n < warnings.size(); n++) {
          auto& warning = warnings[n];
          message.append("\n ", kj::str(n + 1), ". ", warning->toString(js), "\n");
        }
      } else {
        message.append("\n * ", warnings[0]->toString(js), "\n");
      }
      auto msg = message.toString();
      js.logWarning(msg);
    });
  }

  explicit WarnIfUnusedStream(
      jsg::Lock& js, kj::Own<ReadableStreamSource> inner, IoContext& ioContext)
      : ReadableStreamSourceWrapper(kj::mv(inner)),
        warningAggregator(ioContext.getWarningAggregator(unusedStreamBranchKey,
            [](IoContext& context) { return createWarningAggregator(context); })),
        warningContext(kj::heap<UnusedStreamWarningContext>(js)) {}

  ~WarnIfUnusedStream() {
    if (!wasRead) {
      warningAggregator->add(kj::mv(warningContext));
    }
  }

  kj::Promise<size_t> read(kj::ArrayPtr<kj::byte> buffer, size_t minBytes = 1) override {
    wasRead = true;
    return ReadableStreamSourceWrapper::read(buffer, minBytes);
  }

  kj::Promise<kj::Array<const kj::byte>> readAllBytes(size_t limit) override {
    wasRead = true;
    return ReadableStreamSourceWrapper::readAllBytes(limit);
  }

  kj::Promise<kj::String> readAllText(size_t limit) override {
    wasRead = true;
    return ReadableStreamSourceWrapper::readAllText(limit);
  }

  kj::Promise<DeferredProxy<void>> pumpTo(
      WritableStreamSink& output, EndAfterPump end = EndAfterPump::YES) override {
    wasRead = true;
    return ReadableStreamSourceWrapper::pumpTo(output, end);
  }

  void cancel(kj::Exception reason) override {
    wasRead = true;
    return ReadableStreamSourceWrapper::cancel(kj::mv(reason));
  }

  Tee tee(size_t limit) override {
    wasRead = true;
    return ReadableStreamSourceWrapper::tee(limit);
  }

 private:
  kj::Own<WarningAggregator> warningAggregator;
  kj::Own<UnusedStreamWarningContext> warningContext;
  kj::Own<ReadableStreamSource> inner;
  // Used for tracking if this body was ever used.
  bool wasRead = false;
};

// A ReadableStreamSource implementation that lazily wraps an innner Gzip or Brotli
// encoded AsyncInputStream when the first read() is called, or when pumpTo is called,
// the encoding will be selectively and lazily applied to the inner stream.
class EncodedAsyncInputStream final: public ReadableStreamSourceImpl {
 public:
  EncodedAsyncInputStream(kj::Own<kj::AsyncInputStream> inner, rpc::StreamEncoding encoding)
      : ReadableStreamSourceImpl(kj::mv(inner), encoding) {}

  // Read bytes in identity encoding. If the stream is not already in identity encoding, it will be
  // converted to identity encoding via an appropriate stream wrapper.
  kj::Promise<size_t> read(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    try {
      co_return co_await ReadableStreamSourceImpl::read(buffer, minBytes);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_IF_SOME(translated,
          translateKjException(exception,
              {
                {"gzip compressed stream ended prematurely"_kj,
                  "Gzip compressed stream ended prematurely."_kj},
                {"gzip decompression failed"_kj, "Gzip decompression failed."},
                {"brotli state allocation failed"_kj, "Brotli state allocation failed."},
                {"invalid brotli window size"_kj, "Invalid brotli window size."},
                {"invalid brotli compression level"_kj, "Invalid brotli compression level."},
                {"brotli window size too big"_kj, "Brotli window size too big."},
                {"brotli decompression failed"_kj, "Brotli decompression failed."},
                {"brotli compression failed"_kj, "Brotli compression failed."},
                {"brotli compressed stream ended prematurely"_kj,
                  "Brotli compressed stream ended prematurely."},
              })) {
        kj::throwFatalException(kj::mv(translated));
      } else {
        kj::throwFatalException(kj::mv(exception));
      }
    }
  }

  kj::Maybe<Tee> tryTee(size_t limit) override {
    // Note that if we haven't called read() yet, then the inner stream is still
    // in its original encoding. If read() has been called, however, then the inner
    // stream will be wrapped and will be in identity encoding.
    auto& inner = KJ_ASSERT_NONNULL(getState().tryGet<kj::Own<kj::AsyncInputStream>>());
    auto tee = kj::newTee(kj::mv(inner), limit);
    return Tee{
      .branch1 =
          kj::heap<EncodedAsyncInputStream>(wrapTeeBranch(kj::mv(tee.branches[0])), getEncoding()),
      .branch2 =
          kj::heap<EncodedAsyncInputStream>(wrapTeeBranch(kj::mv(tee.branches[1])), getEncoding()),
    };
  }

  kj::Own<kj::AsyncInputStream> ensureIdentityEncoding(
      kj::Own<kj::AsyncInputStream>&& inner) override {
    auto encoding = getEncoding();
    if (encoding == rpc::StreamEncoding::IDENTITY) {
      return kj::mv(inner);
    }
    setEncoding(rpc::StreamEncoding::IDENTITY);
    return wrap(encoding, kj::mv(inner));
  }

 private:
  static kj::Own<kj::AsyncInputStream> wrap(
      rpc::StreamEncoding encoding, kj::Own<kj::AsyncInputStream> inner) {
    switch (encoding) {
      case rpc::StreamEncoding::IDENTITY: {
        return kj::mv(inner);
      }
      case rpc::StreamEncoding::GZIP: {
        return kj::heap<kj::GzipAsyncInputStream>(*inner).attach(kj::mv(inner));
      }
      case rpc::StreamEncoding::BROTLI: {
        return kj::heap<kj::BrotliAsyncInputStream>(*inner).attach(kj::mv(inner));
      }
    }
    KJ_UNREACHABLE;
  }
};

}  // namespace

kj::Own<ReadableStreamSource> newReadableStreamSourceFromBytes(
    kj::ArrayPtr<const kj::byte> bytes, kj::Maybe<kj::Own<void>> maybeBacking) {
  KJ_IF_SOME(backing, maybeBacking) {
    return newReadableStreamSource(newMemoryInputStream(bytes, kj::mv(backing)));
  }

  auto backing = kj::heapArray<kj::byte>(bytes);
  auto ptr = backing.asPtr();
  auto inner = newMemoryInputStream(ptr, kj::heap(kj::mv(backing)));
  return newReadableStreamSource(kj::mv(inner));
}

kj::Own<ReadableStreamSource> newIoContextWrappedReadableStreamSource(
    IoContext& ioctx, kj::Own<ReadableStreamSource> inner) {
  return kj::heap<NoDeferredProxySource>(kj::mv(inner), ioctx);
}

kj::Own<ReadableStreamSource> newReadableStreamSourceFromProducer(
    kj::Function<kj::Promise<size_t>(kj::ArrayPtr<kj::byte>, size_t)> producer,
    kj::Maybe<uint64_t> expectedLength) {
  return newReadableStreamSource(
      kj::heap<InputStreamFromProducer>(kj::mv(producer), expectedLength));
}

kj::Own<ReadableStreamSource> newClosedReadableStreamSource() {
  return kj::heap<ReadableStreamSourceImpl>();
}

kj::Own<ReadableStreamSource> newErroredReadableStreamSource(kj::Exception exception) {
  return kj::heap<ReadableStreamSourceImpl>(kj::mv(exception));
}

kj::Own<ReadableStreamSource> newReadableStreamSource(kj::Own<kj::AsyncInputStream> inner) {
  return kj::heap<ReadableStreamSourceImpl>(kj::mv(inner));
}

kj::Own<ReadableStreamSource> newWarnIfUnusedReadableStreamSource(
    jsg::Lock& js, IoContext& ioContext, kj::Own<ReadableStreamSource> inner) {
  return kj::heap<WarnIfUnusedStream>(js, kj::mv(inner), ioContext);
}

kj::Own<ReadableStreamSource> newEncodedReadableStreamSource(
    rpc::StreamEncoding encoding, kj::Own<kj::AsyncInputStream> inner) {
  return kj::heap<EncodedAsyncInputStream>(kj::mv(inner), encoding);
}

kj::Own<kj::AsyncInputStream> wrapTeeBranch(kj::Own<kj::AsyncInputStream> branch) {
  return TeeErrorAdapter::wrap(kj::mv(branch));
}

}  // namespace workerd::api::streams
