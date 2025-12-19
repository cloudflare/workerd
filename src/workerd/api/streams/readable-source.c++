#include "readable-source.h"

#include "common.h"
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

#include <bit>

namespace workerd::api::streams {

namespace {
// Used to consume and collect all data from a ReadableSource up to a specified
// limit. Throws if the limit is exceeded before EOF.
class AllReader final {
 public:
  explicit AllReader(ReadableSource& input, size_t limit): input(input), limit(limit) {
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
  ReadableSource& input;
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

// A base class for ReadableSource implementations that provides default
// implementations of some methods.
class ReadableSourceImpl: public ReadableSource {
 public:
  ReadableSourceImpl(kj::Own<kj::AsyncInputStream> input,
      rpc::StreamEncoding encoding = rpc::StreamEncoding::IDENTITY)
      : state(kj::mv(input)),
        encoding(encoding) {}
  ReadableSourceImpl(kj::Exception reason)
      : state(kj::mv(reason)),
        encoding(rpc::StreamEncoding::IDENTITY) {}
  ReadableSourceImpl(): state(Closed()), encoding(rpc::StreamEncoding::IDENTITY) {}
  KJ_DISALLOW_COPY_AND_MOVE(ReadableSourceImpl);
  virtual ~ReadableSourceImpl() noexcept(false) {
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
      WritableSink& output, EndAfterPump end = EndAfterPump::YES) override {
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
        // and the pump itself should not rely on the ReadableSource for any state, it is
        // safe to drop the ReadableSource once the pump operation begins.
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
          .branch1 = newErroredReadableSource(kj::cp(errored)),
          .branch2 = newErroredReadableSource(kj::cp(errored)),
        };
      }
      KJ_CASE_ONEOF(_, Closed) {
        return Tee{
          .branch1 = newClosedReadableSource(),
          .branch2 = newClosedReadableSource(),
        };
      }
      KJ_CASE_ONEOF(stream, kj::Own<kj::AsyncInputStream>) {
        KJ_DEFER(state.init<Closed>());
        KJ_IF_SOME(tee, tryTee(limit)) {
          return kj::mv(tee);
        }

        auto tee = kj::newTee(kj::mv(stream), limit);
        return Tee{
          .branch1 = newReadableSource(wrapTeeBranch(kj::mv(tee.branches[0]))),
          .branch2 = newReadableSource(wrapTeeBranch(kj::mv(tee.branches[1]))),
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
      kj::Own<kj::AsyncInputStream> stream, WritableSink& output, EndAfterPump end) {
    // These are fairly arbitrary but reasonable buffer size choices.

    // Note: this intentionally contains code that is similar to the
    // ReadableSourceKjAdapter::pumpToImpl impl in readable-source-adapter.c++.
    // The optimizations are generally the same but the targets are a bit different
    // (ReadableStream vs. kj::AsyncInputStream).

    static constexpr size_t DEFAULT_BUFFER_SIZE = 16384;
    static constexpr size_t MIN_BUFFER_SIZE = 1024;
    static constexpr size_t MED_BUFFER_SIZE = MIN_BUFFER_SIZE << 6;
    static constexpr size_t MAX_BUFFER_SIZE = MIN_BUFFER_SIZE << 7;
    static constexpr size_t MEDIUM_THRESHOLD = 1048576;
    static_assert(MIN_BUFFER_SIZE < DEFAULT_BUFFER_SIZE);
    static_assert(DEFAULT_BUFFER_SIZE < MED_BUFFER_SIZE);
    static_assert(MED_BUFFER_SIZE < MAX_BUFFER_SIZE);
    static_assert(MAX_BUFFER_SIZE < MEDIUM_THRESHOLD);

    // Determine optimal buffer size based on stream length. If the stream does
    // not report a length, use the default. The logic here is simple: use larger
    // buffer sizes for larger streams to reduce the number of read/write iterations.
    // and smaller buffer sizes for smaller streams to reduce memory usage.
    // If the size is unknown, we defer to a reasonable default.
    size_t bufferSize = DEFAULT_BUFFER_SIZE;
    kj::Maybe<uint64_t> maybeRemaining = stream->tryGetLength();
    KJ_IF_SOME(length, maybeRemaining) {
      // Streams that advertise their length SHOULD always tell the truth.
      // But... on the off change they don't, we'll still try to behave
      // reasonably. At worst we will allocate a backing buffer and
      // perform a single read. If this proves to be a performance issue,
      // we can fall back to strictly enforcing the advertised length.
      if (length <= MEDIUM_THRESHOLD) {
        // When `length` is below the medium threshold, use
        // the nearest power of 2 >= length within the range
        // [MIN_BUFFER_SIZE, MED_BUFFER_SIZE].
        bufferSize = kj::max(MIN_BUFFER_SIZE, std::bit_ceil(length));
        bufferSize = kj::min(MED_BUFFER_SIZE, bufferSize);
      } else {
        // Otherwise, use the biggest buffer.
        bufferSize = MAX_BUFFER_SIZE;
      }
    }

    // We use a double-buffering/pipelining strategy here to try to keep both the read
    // and write operations busy in parallel. While one buffer is being written to the
    // output, the other buffer is being filled with data from the input stream. It does
    // mean that we use a bit more memory in the process but should improve throughput on
    // high-latency streams.
    int currentReadBuf = 0;
    kj::SmallArray<kj::byte, 4 * MIN_BUFFER_SIZE> backing(bufferSize * 2);
    kj::ArrayPtr<kj::byte> buffer[] = {
      backing.first(bufferSize),
      backing.slice(bufferSize),
    };

    // We will use an adaptive minBytes value to try to optimize read sizes based on
    // observed stream behavior. We start with a minBytes set to half the buffer size.
    // As the stream is read, we will adjust minBytes up or down depending on whether
    // the stream is consistently filling the buffer or not.
    size_t minBytes = bufferSize >> 1;

    auto readPromise = readImpl(*stream, buffer[currentReadBuf], minBytes);
    size_t iterationCount = 0;
    bool readFailed = false;

    try {
      while (true) {
        // On each iteration, wait for the read to complete...
        size_t amount;
        {
          KJ_ON_SCOPE_FAILURE(readFailed = true);
          amount = co_await readPromise;
        }
        iterationCount++;

        // If we read less than minBytes, assume EOF.
        if (amount < minBytes) {
          // If any bytes were read...
          if (amount > 0) {
            // Write our final chunk...
            co_await output.write(buffer[currentReadBuf].first(amount));
          }
          // Then break out of the loop.
          break;
        }

        // Set the write buffer to the one we just filled.
        auto writeBuf = buffer[currentReadBuf];

        // Then switch to the other buffer and start the next read.
        currentReadBuf = 1 - currentReadBuf;

        // Maybe adjust minBytes based on how much data we read this iteration.
        if (iterationCount <= 3 || iterationCount % 10 == 0) {
          if (amount == bufferSize) {
            // Stream is filling buffer completely... Use smaller minBytes to
            // increase responsiveness, should produce more reads with less data.
            if (bufferSize >= 4 * DEFAULT_BUFFER_SIZE) {
              // For large buffers (≥64KB), be more aggressive about responsiveness.
              // 25% of a large buffer is still a substantial chunk (e.g., 32KB for 128KB).
              minBytes = bufferSize >> 2;  // 25%
            } else {
              // For smaller buffers, 50% provides better balance, avoiding chunks
              // that are too small for efficient processing (e.g., keeps 16KB → 8KB).
              minBytes = bufferSize >> 1;  // 50%
            }
          } else {
            // Stream didn't fill buffer - likely slower or at natural boundary.
            // Use higher minBytes to accumulate larger chunks and reduce iteration overhead.
            minBytes = (bufferSize >> 2) + (bufferSize >> 1);  // 75%
          }
        }

        // Start our next read operation.
        readPromise = readImpl(*stream, buffer[currentReadBuf], minBytes);

        // Write out the chunk we just read in parallel with the next read.
        // If the write fails, the exception will propagate and cancel the pump,
        // including the read operation. If the read fails, it will be picked
        // up at the start of the next loop iteration.
        co_await output.write(writeBuf.first(amount));
      }
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      if (readFailed) {
        output.abort(kj::cp(exception));
      }
      kj::throwFatalException(kj::mv(exception));
    }

    if (end) {
      co_await output.end();
    }
  }

  static kj::Promise<size_t> readImpl(
      kj::AsyncInputStream& inner, kj::ArrayPtr<kj::byte> buffer, size_t minBytes) {
    KJ_ASSERT(minBytes <= buffer.size());
    try {
      // The read() method on AsyncInputStream will throw an exception on short reads,
      // which is why we're using tryRead() here instead.
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

// A ReadableSource wrapper that prevents deferred proxying. This is useful
// when you expect that the IoContext will need to remain live for the duration
// of the operations on the stream.
class NoDeferredProxySource final: public ReadableSourceWrapper {
 public:
  NoDeferredProxySource(kj::Own<ReadableSource> inner, IoContext& ioctx)
      : ReadableSourceWrapper(kj::mv(inner)),
        ioctx(ioctx) {}

  kj::Promise<size_t> read(kj::ArrayPtr<kj::byte> buffer, size_t minBytes = 1) override {
    auto pending = ioctx.registerPendingEvent();
    co_return co_await getInner().read(buffer, minBytes);
  }

  kj::Promise<DeferredProxy<void>> pumpTo(
      WritableSink& output, EndAfterPump end = EndAfterPump::YES) override {
    auto pending = ioctx.registerPendingEvent();
    auto [proxyTask] = co_await getInner().pumpTo(output, end);
    co_await proxyTask;
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
// A ReadableSource wrapper that emits a warning if it is never read from
// before being destroyed. The warning aggregates multiple instances together and
// prints a single warning message when the associated WarningAggregator is destroyed.
// The message includes a stack trace of where each unused stream was created to
// aid in debugging.
class WarnIfUnusedStream final: public ReadableSourceWrapper {
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

  explicit WarnIfUnusedStream(jsg::Lock& js, kj::Own<ReadableSource> inner, IoContext& ioContext)
      : ReadableSourceWrapper(kj::mv(inner)),
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
    return ReadableSourceWrapper::read(buffer, minBytes);
  }

  kj::Promise<kj::Array<const kj::byte>> readAllBytes(size_t limit) override {
    wasRead = true;
    return ReadableSourceWrapper::readAllBytes(limit);
  }

  kj::Promise<kj::String> readAllText(size_t limit) override {
    wasRead = true;
    return ReadableSourceWrapper::readAllText(limit);
  }

  kj::Promise<DeferredProxy<void>> pumpTo(
      WritableSink& output, EndAfterPump end = EndAfterPump::YES) override {
    wasRead = true;
    return ReadableSourceWrapper::pumpTo(output, end);
  }

  void cancel(kj::Exception reason) override {
    wasRead = true;
    return ReadableSourceWrapper::cancel(kj::mv(reason));
  }

  Tee tee(size_t limit) override {
    wasRead = true;
    return ReadableSourceWrapper::tee(limit);
  }

 private:
  kj::Own<WarningAggregator> warningAggregator;
  kj::Own<UnusedStreamWarningContext> warningContext;
  kj::Own<ReadableSource> inner;
  // Used for tracking if this body was ever used.
  bool wasRead = false;
};

// A ReadableSource implementation that lazily wraps an innner Gzip or Brotli
// encoded AsyncInputStream when the first read() is called, or when pumpTo is called,
// the encoding will be selectively and lazily applied to the inner stream.
class EncodedAsyncInputStream final: public ReadableSourceImpl {
 public:
  EncodedAsyncInputStream(kj::Own<kj::AsyncInputStream> inner, rpc::StreamEncoding encoding)
      : ReadableSourceImpl(kj::mv(inner), encoding) {}

  // Read bytes in identity encoding. If the stream is not already in identity encoding, it will be
  // converted to identity encoding via an appropriate stream wrapper.
  kj::Promise<size_t> read(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    try {
      co_return co_await ReadableSourceImpl::read(buffer, minBytes);
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

kj::Own<ReadableSource> newReadableSourceFromBytes(
    kj::ArrayPtr<const kj::byte> bytes, kj::Maybe<kj::Own<void>> maybeBacking) {
  KJ_IF_SOME(backing, maybeBacking) {
    return newReadableSource(newMemoryInputStream(bytes, kj::mv(backing)));
  }

  auto backing = kj::heapArray<kj::byte>(bytes);
  auto ptr = backing.asPtr();
  auto inner = newMemoryInputStream(ptr, kj::heap(kj::mv(backing)));
  return newReadableSource(kj::mv(inner));
}

kj::Own<ReadableSource> newIoContextWrappedReadableSource(
    IoContext& ioctx, kj::Own<ReadableSource> inner) {
  return kj::heap<NoDeferredProxySource>(kj::mv(inner), ioctx);
}

kj::Own<ReadableSource> newReadableSourceFromProducer(
    kj::Function<kj::Promise<size_t>(kj::ArrayPtr<kj::byte>, size_t)> producer,
    kj::Maybe<uint64_t> expectedLength) {
  return newReadableSource(kj::heap<InputStreamFromProducer>(kj::mv(producer), expectedLength));
}

kj::Own<ReadableSource> newClosedReadableSource() {
  return kj::heap<ReadableSourceImpl>();
}

kj::Own<ReadableSource> newErroredReadableSource(kj::Exception exception) {
  return kj::heap<ReadableSourceImpl>(kj::mv(exception));
}

kj::Own<ReadableSource> newReadableSource(kj::Own<kj::AsyncInputStream> inner) {
  return kj::heap<ReadableSourceImpl>(kj::mv(inner));
}

kj::Own<ReadableSource> newWarnIfUnusedReadableSource(
    jsg::Lock& js, IoContext& ioContext, kj::Own<ReadableSource> inner) {
  return kj::heap<WarnIfUnusedStream>(js, kj::mv(inner), ioContext);
}

kj::Own<ReadableSource> newEncodedReadableSource(
    rpc::StreamEncoding encoding, kj::Own<kj::AsyncInputStream> inner) {
  return kj::heap<EncodedAsyncInputStream>(kj::mv(inner), encoding);
}

kj::Own<kj::AsyncInputStream> wrapTeeBranch(kj::Own<kj::AsyncInputStream> branch) {
  return TeeErrorAdapter::wrap(kj::mv(branch));
}

// =======================================================================================
// MemoryInputStream

namespace {

// A ReadableStreamSource backed by in-memory data that does NOT support deferred proxying.
// This is critical when the backing memory may have V8 heap provenance - if we allowed
// deferred proxying, the IoContext could complete and V8 GC could free the memory while
// the deferred pump is still running, causing a use-after-free.
//
// TODO(soon): The expectation is that this will be update to implement ReadableSource instead
// of ReadableStreamSource as we continue the transition.
class MemoryInputStream final: public ReadableStreamSource {
 public:
  MemoryInputStream(kj::ArrayPtr<const kj::byte> bytes, kj::Maybe<kj::Own<void>> backing)
      : unread(bytes),
        backing(kj::mv(backing)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    size_t amount = kj::min(maxBytes, unread.size());
    if (amount > 0) {
      memcpy(buffer, unread.begin(), amount);
      unread = unread.slice(amount, unread.size());
    }
    return amount;
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    if (encoding == StreamEncoding::IDENTITY) {
      return unread.size();
    }
    return kj::none;
  }

  kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end) override {
    // Explicitly NOT using KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING here!
    // The backing memory may be tied to V8 heap (e.g., jsg::BackingStore, Blob data),
    // so we must complete all I/O before the IoContext can be released.
    if (unread.size() > 0) {
      auto data = unread;
      unread = nullptr;
      co_await output.write(data);
    }
    if (end) {
      co_await output.end();
    }
    co_return;
  }

  void cancel(kj::Exception reason) override {
    // Nothing to do - we're just reading from memory.
    unread = nullptr;
  }

 private:
  kj::ArrayPtr<const kj::byte> unread;
  kj::Maybe<kj::Own<void>> backing;
};

}  // namespace

kj::Own<ReadableStreamSource> newMemorySource(
    kj::ArrayPtr<const kj::byte> bytes, kj::Maybe<kj::Own<void>> maybeBacking) {
  KJ_IF_SOME(backing, maybeBacking) {
    return kj::heap<MemoryInputStream>(bytes, kj::mv(backing));
  }
  // No backing provided - make a copy of the bytes.
  auto copy = kj::heapArray<kj::byte>(bytes);
  auto ptr = copy.asPtr();
  return kj::heap<MemoryInputStream>(ptr, kj::heap(kj::mv(copy)));
}

}  // namespace workerd::api::streams
