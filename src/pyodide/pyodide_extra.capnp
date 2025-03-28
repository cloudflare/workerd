@0x96c290dbf479ac0c;

const pythonEntrypoint :Text = embed "python-entrypoint.js";
struct PackageLock {
  packageDate @0 :Text;
  lock @1 :Text;
}
const packageLocks :List(PackageLock) = [
  %PACKAGE_LOCKS
];
