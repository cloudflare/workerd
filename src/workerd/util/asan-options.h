namespace edgeworker {

constexpr const char* ASAN_DEFAULT_OPTIONS =
    "abort_on_error=true detect_leaks=false allow_user_poisoning=false";
}  // namespace edgeworker

#define SANITIZER_HOOK_ATTRIBUTE                                           \
  extern "C"                                                               \
  __attribute__((no_sanitize("address", "memory", "thread", "undefined"))) \
  __attribute__((visibility("default")))                                   \
  __attribute__((retain, used))

// SANITIZER_HOOK_ATTRIBUTE instead of just `extern "C"` solely to make the
// symbol externally visible, and to prevent the linker/compiler from optimizing out any sanitizer
// hooks made.
