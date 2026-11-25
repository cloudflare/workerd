@0xb3f4c7e21a9d5e60;

# The case services of startup-snapshot-globals-<group>-test.wd-test, one per case:
# each runs startup-snapshot-globals-test.js with MODE set to the case's name. See
# startup-snapshot-globals-test.js for what each case does and the runner asserts.

using Workerd = import "/workerd/workerd.capnp";

const compatibilityFlags :List(Text) = [
  "nodejs_compat",
  "typescript_implemented_streams",
  "experimental",
  "unsafe_module",
];

const temporalCompatibilityFlags :List(Text) = [
  "nodejs_compat",
  "typescript_implemented_streams",
  "experimental",
  "unsafe_module",
  "temporal",
];

const nodeCompatV2Flags :List(Text) = [
  "nodejs_compat",
  "nodejs_compat_v2",
  "enable_nodejs_process_v2",
  "typescript_implemented_streams",
  "experimental",
  "unsafe_module",
];

# The variants' oldest compatibility date predates expose_global_message_channel. The case compares
# the globals with what node:worker_threads hands out.
const messageChannelFlags :List(Text) = [
  "nodejs_compat",
  "typescript_implemented_streams",
  "experimental",
  "unsafe_module",
  "expose_global_message_channel",
  "enable_nodejs_worker_threads_module",
];

const control :Workerd.Service = (
  name = "control",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"control\""),
    ],
  )
);

const stlSet :Workerd.Service = (
  name = "stl-set",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"stl-set\""),
    ],
  )
);

const stlSaveRaiseRestore :Workerd.Service = (
  name = "stl-save-raise-restore",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"stl-save-raise-restore\""),
    ],
  )
);

const stlDelete :Workerd.Service = (
  name = "stl-delete",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"stl-delete\""),
    ],
  )
);

const wasmDelete :Workerd.Service = (
  name = "wasm-delete",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"wasm-delete\""),
    ],
  )
);

const temporalProbe :Workerd.Service = (
  name = "temporal-probe",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"temporal-probe\""),
    ],
  )
);

const temporalOnControl :Workerd.Service = (
  name = "temporal-on-control",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .temporalCompatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"temporal-on-control\""),
    ],
  )
);

const quotaExceededErrorPolyfill :Workerd.Service = (
  name = "quota-exceeded-error-polyfill",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"quota-exceeded-error-polyfill\""),
    ],
  )
);

const processAssign :Workerd.Service = (
  name = "process-assign",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .nodeCompatV2Flags,
    bindings = [
      (name = "MODE", json = "\"process-assign\""),
    ],
  )
);

const bufferAssign :Workerd.Service = (
  name = "buffer-assign",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .nodeCompatV2Flags,
    bindings = [
      (name = "MODE", json = "\"buffer-assign\""),
    ],
  )
);

const processEnv :Workerd.Service = (
  name = "process-env",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .nodeCompatV2Flags,
    bindings = [
      (name = "MODE", json = "\"process-env\""),
    ],
  )
);

const processStdout :Workerd.Service = (
  name = "process-stdout",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .nodeCompatV2Flags,
    bindings = [
      (name = "MODE", json = "\"process-stdout\""),
    ],
  )
);

const emptyTypedArray :Workerd.Service = (
  name = "empty-typed-array",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"empty-typed-array\""),
    ],
  )
);

const messageChannel :Workerd.Service = (
  name = "message-channel",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .messageChannelFlags,
    bindings = [
      (name = "MODE", json = "\"message-channel\""),
    ],
  )
);

const messageChannelRead :Workerd.Service = (
  name = "message-channel-read",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .messageChannelFlags,
    bindings = [
      (name = "MODE", json = "\"message-channel-read\""),
    ],
  )
);

const stlNonconfigurable :Workerd.Service = (
  name = "stl-nonconfigurable",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"stl-nonconfigurable\""),
    ],
  )
);

const errorFrozen :Workerd.Service = (
  name = "error-frozen",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"error-frozen\""),
    ],
  )
);

const iterAssign :Workerd.Service = (
  name = "iter-assign",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"iter-assign\""),
    ],
  )
);

const iterDefine :Workerd.Service = (
  name = "iter-define",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"iter-define\""),
    ],
  )
);

const wasmProbe :Workerd.Service = (
  name = "wasm-probe",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"wasm-probe\""),
    ],
  )
);

const wasmProbeCaught :Workerd.Service = (
  name = "wasm-probe-caught",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"wasm-probe-caught\""),
    ],
  )
);

const wasmAssign :Workerd.Service = (
  name = "wasm-assign",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"wasm-assign\""),
    ],
  )
);

const wasmAssignCaught :Workerd.Service = (
  name = "wasm-assign-caught",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"wasm-assign-caught\""),
    ],
  )
);

const iterPolyfillCaught :Workerd.Service = (
  name = "iter-polyfill-caught",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"iter-polyfill-caught\""),
    ],
  )
);

const wasmUse :Workerd.Service = (
  name = "wasm-use",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"wasm-use\""),
    ],
  )
);

const sabProbe :Workerd.Service = (
  name = "sab-probe",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"sab-probe\""),
    ],
  )
);

const temporalPolyfill :Workerd.Service = (
  name = "temporal-polyfill",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"temporal-polyfill\""),
    ],
  )
);

const temporalOnProbe :Workerd.Service = (
  name = "temporal-on-probe",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .temporalCompatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"temporal-on-probe\""),
    ],
  )
);

const dateFrozen :Workerd.Service = (
  name = "date-frozen",
  worker = (
    modules = [(name = "worker", esModule = embed "startup-snapshot-globals-test.js")],
    compatibilityFlags = .compatibilityFlags,
    bindings = [
      (name = "MODE", json = "\"date-frozen\""),
    ],
  )
);
