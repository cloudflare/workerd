# src/workerd/util/

## OVERVIEW

Shared utility library: data structures, SQLite wrapper, feature gating, logging, thread-local scopes. No workerd-specific API dependencies — consumed across `api/`, `io/`, `server/`.

## KEY UTILITIES

| File              | What                                                     | Notes                                                                                  |
| ----------------- | -------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| `state-machine.h` | Type-safe `kj::OneOf` wrapper with transition locking    | Prevents UAF in callbacks; 7 consumers in `api/streams/`                               |
| `sqlite.h/c++`    | Full SQLite wrapper with custom VFS over `kj::Directory` | `Regulator` controls allowed SQL; `Statement` for prepared queries                     |
| `sqlite-kv.h`     | KV abstraction on SQLite                                 | Used by Durable Object storage                                                         |
| `autogate.h/c++`  | Runtime feature gates                                    | `AutogateKey` enum; `isEnabled()` check; 8 active gates                                |
| `weak-refs.h`     | `WeakRef<T>` / `AtomicWeakRef<T>`                        | Non-owning refs; `tryAddStrongRef()` or `runIfAlive(fn)` pattern                       |
| `ring-buffer.h`   | Amortized O(1) deque                                     | Replaces `std::list` in streams                                                        |
| `small-set.h`     | `kj::OneOf`-based set                                    | O(1) for 0–2 items, fallback to `kj::HashSet`                                          |
| `batch-queue.h`   | Double-buffered cross-thread queue                       | Producer/consumer with mutex swap                                                      |
| `checked-queue.h` | Safe `std::list` wrapper                                 | `pop()` returns `Maybe` instead of UB on empty                                         |
| `strong-bool.h`   | `WD_STRONG_BOOL(Name)` macro                             | Type-safe boolean; prevents implicit conversions                                       |
| `sentry.h`        | `LOG_EXCEPTION`, `LOG_ONCE`, `LOG_PERIODICALLY`          | `DEBUG_FATAL_RELEASE_LOG` = debug assert + release warning                             |
| `thread-scopes.h` | Thread-local scope flags                                 | Self-described "horrible hacks"; `AllowV8BackgroundThreadsScope`, `MultiTenantProcess` |
| `abortable.h`     | `newAbortableInputStream/OutputStream`                   | Wraps KJ streams with disconnect capability                                            |
| `stream-utils.h`  | `NeuterableInputStream`, `newNullInputStream`            | Disconnectable I/O; null/identity stream factories                                     |
| `mimetype.h`      | MIME type parser/serializer                              | `MimeType::extract()` from content-type header                                         |
| `wait-list.h`     | Cross-request event subscription                         | Shared fulfiller list for signaling waiters                                            |

## ANTI-PATTERNS

- **StateMachine `forceTransitionTo()`**: bypasses terminal state protection — use only for error recovery
- **StateMachine `underlying()`**: bypasses ALL safety (transition lock, terminal states) — last resort only
- **StateMachine + `KJ_SWITCH_ONEOF`**: does NOT acquire transition lock — UAF risk; use `whenState<T>(fn)` instead
- **StateMachine `deferTransitionTo()`**: first-wins semantics; second call silently ignored
- **SQLite**: `SQLITE_MISUSE` always throws; virtual tables disallowed (except FTS5); `ATTACH`/`DETACH` forbidden; callbacks must not write
- **Autogate**: enum in `.h` and string map in `.c++` MUST stay in sync — add to both or get silent mismatch
- **ThreadScopes**: thread-local state crossing module boundaries — acknowledged hack, do not proliferate
