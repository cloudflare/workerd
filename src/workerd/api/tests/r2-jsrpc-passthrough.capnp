# Shared pieces of the R2 JSRPC passthrough topology, imported by the R2 JSRPC configs. This file
# is not a test config itself. embed paths resolve relative to this file.
#
#   user --JSRPC--> r2-test (ObservedR2Binding) --HTTP via native REAL_BUCKET--> r2-test (fetch)
#
# The r2-test service runs ObservedR2Binding without r2_binding_jsrpc, so its native REAL_BUCKET
# uses the HTTP protocol. REAL_BUCKET points back at the same service, whose default export is
# the HTTP mock from r2-test.js. Importing configs must therefore name the service "r2-test",
# which is also the bucket name that R2 spans record.
#
# Each worker variant matches the mock's R2_LIST_HONOR_INCLUDE expectation to its
# r2_list_honor_include flag.

@0xe8a01684ab8486bc;

using Workerd = import "/workerd/workerd.capnp";

# Modules for a user Worker that runs the canonical and shared suites.
const userModules :List(Workerd.Worker.Module) = [
  ( name = "worker",
    esModule = "export { default, deletePerKeyErrorParityTests, r2ValidationTests, r2BindingApiTests, r2MultipartApiTests } from './r2-test.js';" ),
  ( name = "r2-test.js", esModule = embed "r2-test.js" ),
];

# Exports ObservedR2Binding and the HTTP mock, but no test handlers, so the test runner does not
# run the tests exported by r2-test.js in this service.
const passthroughModules :List(Workerd.Worker.Module) = [
  ( name = "worker",
    esModule = "import mock, { ObservedR2Binding } from './r2-test.js'; export { ObservedR2Binding }; export default { fetch: mock.fetch };" ),
  ( name = "r2-test.js", esModule = embed "r2-test.js" ),
];

# r2_list_honor_include with slow structs.
const passthroughWorker :Workerd.Worker = (
  modules = .passthroughModules,
  bindings = [
    ( name = "REAL_BUCKET", r2Bucket = "r2-test" ),
    ( name = "R2_LIST_HONOR_INCLUDE", text = "true" ),
  ],
  compatibilityFlags = ["nodejs_compat", "streams_enable_constructors",
                        "r2_list_honor_include", "disable_fast_jsg_struct"],
);

# No r2_list_honor_include, with slow structs.
const passthroughLegacyListWorker :Workerd.Worker = (
  modules = .passthroughModules,
  bindings = [
    ( name = "REAL_BUCKET", r2Bucket = "r2-test" ),
    ( name = "R2_LIST_HONOR_INCLUDE", text = "false" ),
  ],
  compatibilityFlags = ["nodejs_compat", "streams_enable_constructors",
                        "disable_fast_jsg_struct"],
);

# No r2_list_honor_include, with fast structs.
const passthroughFastStructsWorker :Workerd.Worker = (
  modules = .passthroughModules,
  bindings = [
    ( name = "REAL_BUCKET", r2Bucket = "r2-test" ),
    ( name = "R2_LIST_HONOR_INCLUDE", text = "false" ),
  ],
  compatibilityFlags = ["nodejs_compat", "streams_enable_constructors",
                        "enable_fast_jsg_struct"],
);
