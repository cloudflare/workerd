// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/one-of.h>

#include <coroutine>

struct Value {};
void consume(Value&&);
void use(const Value&);
bool choose();

using Choice = kj::OneOf<Value, int>;

void straightLine(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      consume(kj::mv(value));
      use(value);  // expect-warning
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void onlyMove(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      consume(kj::mv(value));
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void unevaluatedAfterMove(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      consume(kj::mv(value));
      (void)sizeof(value);
      (void) noexcept(use(value));
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void nestedMove(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      if (choose()) {
        consume(kj::mv(value));
      }
      use(value);  // expect-warning
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void exclusivePaths(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      if (choose()) {
        consume(kj::mv(value));
      } else {
        use(value);
      }
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void reinitialized(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      consume(kj::mv(value));
      value = Value();
      use(value);
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void innerLoop(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      while (choose()) {
        consume(kj::mv(value));  // expect-loop-warning
      }
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void outerLoop(Choice choice, Value other) {
  while (choose()) {
    KJ_SWITCH_ONEOF(choice) {
      KJ_CASE_ONEOF(value, Value) {
        consume(kj::mv(other));  // expect-loop-warning
      }
      KJ_CASE_ONEOF(value, int) {}
    }
  }
}

void afterCase(Choice choice, Value other) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      consume(kj::mv(other));
    }
    KJ_CASE_ONEOF(value, int) {}
  }
  use(other);  // expect-warning
}

void continueCase(Choice choice, Value other) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      consume(kj::mv(other));
      continue;
    }
    KJ_CASE_ONEOF(value, int) {}
  }
  use(other);  // expect-warning
}

void breakCase(Choice choice, Value other) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      consume(kj::mv(other));
      break;
    }
    KJ_CASE_ONEOF(value, int) {}
  }
  use(other);  // expect-warning
}

void exclusiveCases(Choice choice, Value other) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      consume(kj::mv(other));
    }
    KJ_CASE_ONEOF(value, int) {
      use(other);
    }
  }
}

void nestedCases(Choice first, Choice second, Value other) {
  KJ_SWITCH_ONEOF(first) {
    KJ_CASE_ONEOF(value, Value) {
      KJ_SWITCH_ONEOF(second) {
        KJ_CASE_ONEOF(inner, Value) {
          consume(kj::mv(other));
        }
        KJ_CASE_ONEOF(inner, int) {}
      }
      use(other);  // expect-warning
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

#define WRAPPED_CASE(name, type) KJ_CASE_ONEOF(name, type)

void wrappedCase(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    WRAPPED_CASE(value, Value) {
      consume(kj::mv(value));
      (void)sizeof(value);
    }
    WRAPPED_CASE(value, int) {}
  }
}

void userForInCase(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      for (int i = 0; i < 2; ++i) {
        consume(kj::mv(value));  // expect-loop-warning
      }
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void lambdaInCase(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      auto callback = [value = kj::mv(value)]() mutable {
        consume(kj::mv(value));
        use(value);  // expect-warning
      };
      callback();
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void useBeforeMove(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      use(value);
      consume(kj::mv(value));
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void unreachableUse(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      consume(kj::mv(value));
      if (false) use(value);
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void moveBeforeCase(Choice choice, Value other) {
  consume(kj::mv(other));
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      use(other);  // expect-warning
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void commaUse(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      (consume(kj::mv(value)), use(value));  // expect-warning
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void gotoInCase(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
    again:
      consume(kj::mv(value));  // expect-loop-warning
      if (choose()) goto again;
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

void continueWithoutUse(Choice choice) {
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      consume(kj::mv(value));
      continue;
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

struct Awaitable {
  bool await_ready() const;
  void await_suspend(std::coroutine_handle<>) const;
  void await_resume() const;
};

struct Promise {
  struct promise_type {
    Promise get_return_object();
    std::suspend_never initial_suspend();
    std::suspend_never final_suspend() noexcept;
    void return_void();
    void unhandled_exception();
  };
};

template <typename Func>
Awaitable run(Func&&) {
  return Awaitable();
}

Promise coroutineCaptureInCase(Choice choice) {
  co_await Awaitable();
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      co_await run([value = kj::mv(value)] { use(value); });
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

Promise useAfterCoroutineCapture(Choice choice) {
  co_await Awaitable();
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      co_await run([value = kj::mv(value)] { use(value); });
      use(value);  // expect-warning
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}

Promise useAfterMoveInCoroutineCapture(Choice choice) {
  co_await Awaitable();
  KJ_SWITCH_ONEOF(choice) {
    KJ_CASE_ONEOF(value, Value) {
      co_await run([value = kj::mv(value)]() mutable {
        consume(kj::mv(value));
        use(value);  // expect-warning
      });
    }
    KJ_CASE_ONEOF(value, int) {}
  }
}
