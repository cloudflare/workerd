@0x96c290dbf479ac0c;
using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd");

struct PackageLock {
  packageDate @0 :Text;
  lock @1 :Text;
}
const packageLocks :List(PackageLock) = [
  %PACKAGE_LOCKS
];


struct PythonSnapshotRelease @0x89c66fb883cb6975 {
  # Used to indicate a specific Python release that introduces a change which likely breaks
  # existing memory snapshots.
  #
  # The versions/dates specified here are used to generate a filename for the package memory
  # snapshots created by the validator. They are also used to generate a filename of the Pyodide
  # and package bundle that gets downloaded for Python Workers.
  pyodide @0 :Text;
  # The Pyodide version, for example "0.26.0a2".
  pyodideRevision @1 :Text;
  # A date identifying a revision of the above Pyodide version. A change in this field but not
  # the `pyodide` version field may indicate that changes to the workerd Pyodide integration code
  # were made with the Pyodide version remaining the same.
  #
  # For example "2024-05-25".
  packages @2 :Text;
  # A date identifying a revision of the Python package bundle.
  #
  # For example "2024-02-18".
  backport @3 :Int64;
  # A number that is incremented each time we need to backport a fix to an existing Python release.
  baselineSnapshotHash @4 :Text;
  # A sha256 checksum hash of the baseline/universal memory snapshot to use for Python Workers using
  # this release.
  fieldName @5 :Text;
  # Name of the corresponding feature flag
  flagName @6 :Text;
  realPyodideVersion @7 :Text;
}

const releases :List(PythonSnapshotRelease) = [
  %PYTHON_RELEASES
];
