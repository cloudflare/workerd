#include "readable-source.h"

#include "writable-sink.h"

#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/stream-utils.h>
#include <workerd/util/string-buffer.h>

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
    // There are a few complexities in this operation that make it difficult to completely
    // optimize. The most important is that even if a stream reports an expected length
    // using tryGetLength, we really don't know how much data the stream will produce until
    // we try to read it. The only signal we have that the stream is done producing data
    // is a zero-length result from tryRead. Unfortunately, we have to allocate a buffer
    // in advance of calling tryRead so we have to guess a bit at the size of the buffer
    // to allocate.
    //
    // In the previous implementation of this method, we would just blindly allocate a
    // 4096 byte buffer on every allocation, limiting each read iteration to a maximum
    // of 4096 bytes. This works fine for streams producing a small amount of data but
    // risks requiring a greater number of loop iterations and small allocations for streams
    // that produce larger amounts of data. Also in the previous implementation, every
    // loop iteration would allocate a new buffer regardless of how much of the previous
    // allocation was actually used -- so a stream that produces only 4000 bytes total
    // but only provides 10 bytes per iteration would end up with 400 reads and 400 4096
    // byte allocations. Doh! Fortunately our stream implementations tend to be a bit
    // smarter than that but it's still a worst case possibility that it's likely better
    // to avoid.
    //
    // So this implementation does things a bit differently.
    // First, we check to see if the stream can give an estimate on how much data it
    // expects to produce. If that length is within a given threshold, then best case
    // is we can perform the entire read with at most two allocations and two calls to
    // tryRead. The first allocation will be for the entire expected size of the stream,
    // which the first tryRead will attempt to fulfill completely. In the best case the
    // stream provides all of the data. The next allocation would be smaller and would
    // end up resulting in a zero-length read signaling that we are done. Hooray!
    //
    // Not everything can be best case scenario tho, unfortunately. If our first tryRead
    // does not fully consume the stream or fully fill the destination buffer, we're
    // going to need to try again. It is possible that the new allocation in the next
    // iteration will be wasted if the stream doesn't have any more data so it's important
    // for us to try to be conservative with the allocation. If the running total of data
    // we've seen so far is equal to or greater than the expected total length of the stream,
    // then the most likely case is that the next read will be zero-length -- but unfortunately
    // we can't know for sure! So for this we will fall back to a more conservative allocation
    // which is either 4096 bytes or the calculated amountToRead, whichever is the lower number.

    kj::Vector<kj::Array<T>> parts;
    size_t runningTotal = 0;
    static constexpr size_t MIN_BUFFER_CHUNK = 1024;
    static constexpr size_t DEFAULT_BUFFER_CHUNK = 4096;
    static constexpr size_t MAX_BUFFER_CHUNK = DEFAULT_BUFFER_CHUNK * 4;

    // If we know in advance how much data we'll be reading, then we can attempt to optimize the
    // loop here by setting the value specifically so we are only allocating at most twice. But,
    // to be safe, let's enforce an upper bound on each allocation even if we do know the total.
    kj::Maybe<size_t> maybeLength = input.tryGetLength(rpc::StreamEncoding::IDENTITY);

    // The amountToRead is the regular allocation size we'll use right up until we've read the
    // number of expected bytes (if known). This number is calculated as the minimum of (limit,
    // MAX_BUFFER_CHUNK, maybeLength or DEFAULT_BUFFER_CHUNK). In the best case scenario, this
    // number is calculated such that we can read the entire stream in one go if the amount of
    // data is small enough and the stream is well behaved.
    //
    // If the stream does report a length, once we've read that number of bytes, we'll
    // fallback to the more conservative allocation.
    size_t amountToRead =
        kj::min(limit, kj::min(MAX_BUFFER_CHUNK, maybeLength.orDefault(DEFAULT_BUFFER_CHUNK)));

    // amountToRead can be zero if the stream reported a zero-length. While the stream could
    // be lying about its length, let's skip reading anything in this case.
    if (amountToRead != 0) {
      while (true) {
        auto bytes = kj::heapArray<T>(amountToRead);
        // Note that we're passing amountToRead as the *minBytes* here so the tryRead should
        // attempt to fill the entire buffer. If it doesn't, the implication is that we read
        // everything.
        size_t amount = co_await input.read(bytes.asBytes(), amountToRead);
        KJ_DASSERT(amount <= amountToRead);

        runningTotal += amount;
        JSG_REQUIRE(runningTotal < limit, TypeError, "Memory limit exceeded before EOF.");

        if (amount < amountToRead) {
          // The stream has indicated that we're all done by returning a value less than the
          // full buffer length.
          // It is possible/likely that at least some amount of data was written to the buffer.
          // In which case we want to add that subset to the parts list here before we exit
          // the loop.
          if (amount > 0) {
            parts.add(bytes.first(amount).attach(kj::mv(bytes)));
          }
          break;
        }

        // Because we specify minSize equal to maxSize in the tryRead above, we should only
        // get here if the buffer was completely filled by the read. If it wasn't completely
        // filled, that is an indication that the stream is complete which is handled above.
        KJ_DASSERT(amount == bytes.size());
        parts.add(kj::mv(bytes));

        // If the stream provided an expected length and our running total is equal to or
        // greater than that length then we'll adjust our allocation strategy to be more
        // conservative since the next read is likely (hopefully) going to be zero-length.
        // Worst case, the stream is lying about its length and we have to keep reading.
        // Best case, the next read is zero-length and we only had to do one additional
        // small allocation.
        KJ_IF_SOME(length, maybeLength) {
          if (runningTotal >= length) {
            amountToRead = kj::min(MIN_BUFFER_CHUNK, amountToRead);
            continue;
          }
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
class DelegatingInputStream final: public kj::AsyncInputStream {
 public:
  using Producer = kj::Function<kj::Promise<size_t>(kj::ArrayPtr<kj::byte>, size_t)>;
  DelegatingInputStream(Producer producer, kj::Maybe<uint64_t> expectedLength)
      : producer(kj::mv(producer)),
        expectedLength(expectedLength) {}

  KJ_DISALLOW_COPY_AND_MOVE(DelegatingInputStream);

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

  kj::Promise<size_t> read(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    KJ_REQUIRE(canceler.isEmpty(), "jsg.Error: Stream is already being read");
    co_return co_await canceler.wrap(readImpl(buffer, minBytes, {.identityEncoding = true}));
  }

  kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end) override {
    if (!canceler.isEmpty()) {
      return kj::Promise<DeferredProxy<void>>(
          DeferredProxy<void>{KJ_EXCEPTION(FAILED, "jsg.Error: Stream is already being read")});
    }
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(inner, kj::Own<kj::AsyncInputStream>) {
        if (output.getEncoding() != getEncoding()) {
          // The target encoding is different from our current encoding.
          // Let's ensure that our side is in identity encoding. The destination stream will
          // take care of itself.
          ensureIdentityEncoding(inner);
        } else {
          // Since the encodings match, we can tell the output stream that it doesn't need to
          // do any of the encoding work since we'll be providing data in the expected encoding.
          KJ_ASSERT(getEncoding() == output.disownEncodingResponsibility());
        }

        // By default, we assume the pump is eligible for deferred proxying.
        return kj::Promise<DeferredProxy<void>>(
            DeferredProxy<void>{canceler.wrap(pumpImpl(output, end))});
      }
      KJ_CASE_ONEOF(closed, Closed) {
        if (end) {
          return kj::Promise<DeferredProxy<void>>(DeferredProxy<void>{
            .proxyTask = output.end(),
          });
        } else {
          return kj::Promise<DeferredProxy<void>>(DeferredProxy<void>{
            .proxyTask = kj::READY_NOW,
          });
        }
      }
      KJ_CASE_ONEOF(errored, kj::Exception) {
        output.abort(kj::cp(errored));
        return kj::Promise<DeferredProxy<void>>(DeferredProxy<void>{.proxyTask = kj::mv(errored)});
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

  Tee tee(kj::Maybe<size_t> maybeLimit) override {
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
        KJ_IF_SOME(tee, tryTee(maybeLimit)) {
          return kj::mv(tee);
        }

        static constexpr size_t kMax = kj::maxValue;
        auto tee = kj::newTee(kj::mv(stream), maybeLimit.orDefault(kMax));
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

  virtual kj::AsyncInputStream& ensureIdentityEncoding(kj::Own<kj::AsyncInputStream>& inner) {
    // By default, we always use identity encoding so nothing to do here.
    // It is up to subclasses to override this if they support other encodings.
    KJ_DASSERT(encoding == rpc::StreamEncoding::IDENTITY);
    return *inner;
  }

  virtual void prepareRead() {
    // Do nothing by default.
  };

  // Implementations should override to provide an alternative tee implementation.
  // This will only be called when the state is known to be not closed or errored.
  virtual kj::Maybe<Tee> tryTee(kj::Maybe<size_t> maybeLimit) {
    return kj::none;
  }

  // The default non-optimized pumpTo() implementation which initiates a loop
  // that reads a chunk from the input stream and writes it to the output
  // stream until EOF is reached. The maximum size of each read is 16384 bytes.
  // The pump is canceled by dropping the returned.
  virtual kj::Promise<void> pumpImpl(WritableStreamSink& output, bool end) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(errored, kj::Exception) {
        kj::throwFatalException(kj::cp(errored));
      }
      KJ_CASE_ONEOF(_, Closed) {
        if (end) {
          co_await output.end();
        }
        co_return;
      }
      KJ_CASE_ONEOF(_, kj::Own<kj::AsyncInputStream>) {
        kj::FixedArray<kj::byte, 16384> buffer;
        static constexpr size_t N = buffer.size();
        static constexpr size_t kMinBytes = (N >> 2) + (N >> 1);  // 3/4 of N
        while (true) {
          // It's most likely that our write below is potentially a write into
          // a JS-backed stream which requires grabbing the isolate lock.
          // To minimize the number of times we need to grab the lock, we
          // want to read as much data as we can here before doing the write.
          // We obviously need to balance that with not waiting too long
          // between writes. We'll set our minBytes to 3/4 of the buffer size
          // to try to strike a balance.
          auto amount = co_await readImpl(buffer, kMinBytes);
          // If the amount is less than kMinBytes, we assume we've reached EOF.

          if (amount > 0) {
            co_await output.write(buffer.asPtr().first(amount));
          }

          if (amount < kMinBytes) {
            if (end) {
              co_await output.end();
            }
            co_return;
          }
        }
      }
    }
    KJ_UNREACHABLE;
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

  kj::Promise<size_t> readImpl(kj::ArrayPtr<kj::byte> buffer,
      size_t minBytes,
      ReadOption option = {.identityEncoding = false}) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(errored, kj::Exception) {
        kj::throwFatalException(kj::cp(errored));
      }
      KJ_CASE_ONEOF(_, Closed) {
        co_return 0;
      }
      KJ_CASE_ONEOF(inner, kj::Own<kj::AsyncInputStream>) {
        KJ_ASSERT(minBytes <= buffer.size());
        kj::AsyncInputStream& stream =
            option.identityEncoding ? ensureIdentityEncoding(inner) : *inner;
        try {
          // The read() method on AsyncInputStream will throw an exception on short reads.
          auto amount = co_await stream.tryRead(buffer.begin(), minBytes, buffer.size());
          if (amount < minBytes) {
            setClosed();
          }
          co_return amount;
        } catch (...) {
          auto exception = kj::getCaughtExceptionAsKj();
          if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
            // Treat disconnect as EOF.
            setClosed();
            co_return 0;
          }
          setErrored(kj::cp(exception));
          kj::throwFatalException(kj::mv(exception));
        }
      }
    }
    KJ_UNREACHABLE;
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

  kj::Promise<size_t> read(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    auto pending = ioctx.registerPendingEvent();
    co_return co_await getInner().read(buffer, minBytes);
  }

  kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end) override {
    return addNoopDeferredProxy(ioctx.waitForDeferredProxy(getInner().pumpTo(output, end)));
  }

  Tee tee(kj::Maybe<size_t> limit) override {
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

  kj::Promise<size_t> read(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
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

  kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end) override {
    wasRead = true;
    return ReadableStreamSourceWrapper::pumpTo(output, end);
  }

  void cancel(kj::Exception reason) override {
    wasRead = true;
    return ReadableStreamSourceWrapper::cancel(kj::mv(reason));
  }

  Tee tee(kj::Maybe<size_t> limit) override {
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

  kj::Maybe<Tee> tryTee(kj::Maybe<size_t> maybeLimit) override {
    // Note that if we haven't called read() yet, then the inner stream is still
    // in its original encoding. If read() has been called, however, then the inner
    // stream will be wrapped and will be in identity encoding.
    static constexpr size_t kMax = kj::maxValue;
    auto& inner = KJ_ASSERT_NONNULL(getState().tryGet<kj::Own<kj::AsyncInputStream>>());
    auto tee = kj::newTee(kj::mv(inner), maybeLimit.orDefault(kMax));
    return Tee{
      .branch1 =
          kj::heap<EncodedAsyncInputStream>(wrapTeeBranch(kj::mv(tee.branches[0])), getEncoding()),
      .branch2 =
          kj::heap<EncodedAsyncInputStream>(wrapTeeBranch(kj::mv(tee.branches[1])), getEncoding()),
    };
  }

  kj::AsyncInputStream& ensureIdentityEncoding(kj::Own<kj::AsyncInputStream>& inner) override {
    auto encoding = getEncoding();
    if (encoding == rpc::StreamEncoding::IDENTITY) return *inner;
    setEncoding(rpc::StreamEncoding::IDENTITY);
    return setStream(wrap(encoding, kj::mv(inner)));
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
    auto inner = newMemoryInputStream(bytes);
    return newReadableStreamSource(inner.attach(kj::mv(backing)));
  }

  auto backing = kj::heapArray<kj::byte>(bytes);
  auto inner = newMemoryInputStream(backing.asPtr());
  return newReadableStreamSource(inner.attach(kj::mv(backing)));
}

kj::Own<ReadableStreamSource> newReadableStreamSourceWithoutDeferredProxy(
    IoContext& ioctx, kj::Own<ReadableStreamSource> inner) {
  return kj::heap<NoDeferredProxySource>(kj::mv(inner), ioctx);
}

kj::Own<ReadableStreamSource> newReadableStreamSourceFromDelegate(
    kj::Function<kj::Promise<size_t>(kj::ArrayPtr<kj::byte>, size_t)> producer,
    kj::Maybe<uint64_t> expectedLength) {
  return newReadableStreamSource(kj::heap<DelegatingInputStream>(kj::mv(producer), expectedLength));
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
