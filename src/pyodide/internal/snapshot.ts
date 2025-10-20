import { enterJaegerSpan } from 'pyodide-internal:jaeger';
import { default as ArtifactBundler } from 'pyodide-internal:artifacts';
import { default as UnsafeEval } from 'internal:unsafe-eval';
import { default as DiskCache } from 'pyodide-internal:disk_cache';
import { type FilePath, VIRTUALIZED_DIR } from 'pyodide-internal:setupPackages';
import { default as EmbeddedPackagesTarReader } from 'pyodide-internal:packages_tar_reader';
import {
  SHOULD_SNAPSHOT_TO_DISK,
  IS_CREATING_BASELINE_SNAPSHOT,
  MEMORY_SNAPSHOT_READER,
  REQUIREMENTS,
  IS_CREATING_SNAPSHOT,
  IS_EW_VALIDATING,
  IS_DEDICATED_SNAPSHOT_ENABLED,
  COMPATIBILITY_FLAGS,
  type CompatibilityFlags,
  IS_SECOND_VALIDATION_PHASE,
} from 'pyodide-internal:metadata';
import {
  invalidateCaches,
  PythonRuntimeError,
  PythonUserError,
  simpleRunPython,
} from 'pyodide-internal:util';
import { default as MetadataReader } from 'pyodide-internal:runtime-generated/metadata';
import type { PyodideEntrypointHelper } from 'pyodide:python-entrypoint-helper';

// A handle is the pointer into the linear memory returned by dlopen. Multiple dlopens will return
// multiple pointers.
type DsoHandles = {
  [name: string]: { handles: number[] };
};

// This is the info about Dsos that we record in getMemoryPatched. Namely the load order and where
// their metadata is allocated. It would be natural to also have dsoHandles in here, but we don't
// need a global variable to calculate dsoHandles since it is calculable from the information that
// Emscripten stores in Module.LDSO (see recordDsoHandles).
type DsoLoadInfo = {
  readonly loadOrder: string[];
  readonly soMemoryBases: { [name: string]: number };
  readonly soTableBases?: { [name: string]: number };
};

// This is the old wire format, where "settings" is mixed with the DsoHandles information and
// DsoLoadInfo is not present because we used to preload all dynamic libraries in a standard order.
// The "version" field is never present in the old wire format, but at runtime accessing it will
// give undefined. We need to include it here so that we can use the version field as the
// descriminator for `OldSnapshotMeta | SnapshotMeta`.
type OldSnapshotMeta = DsoHandles & {
  readonly settings?: { readonly baselineSnapshot?: boolean };
  readonly version?: undefined;
};

type LoadedSnapshotSettings = {
  readonly snapshotType: ArtifactBundler.SnapshotType;
  readonly compatFlags: CompatibilityFlags;
};

type SnapshotSettings = {
  readonly baselineSnapshot?: boolean;
} & Partial<LoadedSnapshotSettings>;

// The new wire format, with additional information about the hiwire state, the order that dsos were
// loaded in, and their memory bases. We also moved settings out of the dsoHandles.
type SnapshotMeta = {
  // We just store importedModulesList to help with testing and introspection
  readonly importedModulesList: ReadonlyArray<string> | undefined;
  readonly hiwire: SnapshotConfig | undefined;
  readonly dsoHandles: DsoHandles;
  readonly settings: SnapshotSettings;
  readonly version: 1;
} & DsoLoadInfo;

// MEMORY_SNAPSHOT_READER has type SnapshotReader | undefined
type SnapshotReader = {
  readMemorySnapshot: (offset: number, buf: Uint32Array | Uint8Array) => void;
  getMemorySnapshotSize: () => number;
  disposeMemorySnapshot: () => void;
};

// Extra info that isn't stored in the snapshot but is calculated at runtime: snapshotSize and
// snapshotOffset would be awkward to store in the snapshot. snapshotReader is equal to
// MEMORY_SNAPSHOT_READER, but typescript knows it is defined.
type LoadedSnapshotExtras = {
  snapshotSize: number;
  snapshotOffset: number;
  snapshotReader: SnapshotReader;
};

type LoadedSnapshotMeta = SnapshotMeta &
  LoadedSnapshotExtras & { readonly settings: LoadedSnapshotSettings };

/**
 * Constants
 */
// "\x00snp"
const SNAPSHOT_MAGIC = 0x706e7300;
const CREATE_SNAPSHOT_VERSION = 2;
const HEADER_SIZE = 4 * 4;

/**
 * Global variables for the memory snapshot.
 */
const LOADED_SNAPSHOT_META: LoadedSnapshotMeta | undefined = decodeSnapshot(
  MEMORY_SNAPSHOT_READER
);
const CREATED_SNAPSHOT_META: Required<DsoLoadInfo> = {
  soMemoryBases: {},
  soTableBases: {},
  loadOrder: [],
};
if (LOADED_SNAPSHOT_META) {
  // Make sure we include the soMemoryBases and loadOrder from the baseline snapshot when we are
  // generating stacked snapshots.
  Object.assign(
    CREATED_SNAPSHOT_META.soMemoryBases,
    LOADED_SNAPSHOT_META.soMemoryBases
  );
  Object.assign(
    CREATED_SNAPSHOT_META.soTableBases,
    LOADED_SNAPSHOT_META.soTableBases
  );
  CREATED_SNAPSHOT_META.loadOrder.push(...LOADED_SNAPSHOT_META.loadOrder);
}
export const LOADED_SNAPSHOT_TYPE = LOADED_SNAPSHOT_META?.settings.snapshotType;

/**
 * Preload a dynamic library.
 *
 * Emscripten would usually figure out all of these details for us
 * automatically. These defaults work for shared libs that are configured as
 * standard Python extensions. This naive approach will not work for libraries
 * like scipy, shapely, geos...
 * TODO(someday) fix this.
 */
function loadDynlib(
  Module: Module,
  path: string,
  wasmModuleData: Uint8Array
): void {
  const wasmModule = UnsafeEval.newWasmModule(wasmModuleData);
  const dso = Module.newDSO(path, undefined, 'loading');
  // even though these are used via dlopen, we are allocating them in an arena
  // outside the heap and the memory cannot be reclaimed. So I don't think it
  // would help us to allow them to be dealloc'd.
  dso.refcount = Infinity;
  // Hopefully they are used with dlopen
  dso.global = false;
  const options = {};
  // Passing this empty object as dylibLocalScope fixes symbol lookup in dependent shared libraries
  // that are not loaded globally, thus fixing one of our problems with the upstream shift away from
  // RLTD_GLOBAL. Emscripten should probably be updated so that if dylibLocalScope is undefined it
  // will give the dynamic library a new empty loading scope.
  const dylibLocalScope = {};
  dso.exports = Module.loadWebAssemblyModule(
    wasmModule,
    options,
    path,
    dylibLocalScope
  );
  // "handles" are dlopen handles. There will be one entry in the `handles` list
  // for each dlopen handle that has not been dlclosed. We need to keep track of
  // these across
  const { handles } = LOADED_SNAPSHOT_META?.dsoHandles[path] || { handles: [] };
  for (const handle of handles) {
    Module.LDSO.loadedLibsByHandle[handle] = dso;
  }
  Module.LDSO.loadedLibsByName[path.split('/').at(-1)!] = dso;
}

/**
 * This function is used to ensure the order in which we load SO_FILES stays the same. It is only
 * used for 0.26.0a2, later we look at SNAPSHOT_META.loadOrder to decide what order to load libs.
 *
 * The sort always puts _lzma.so and _ssl.so first, because these SO_FILES are loaded in the
 * baseline snapshot, and if we want to generate a package snapshot while a baseline snapshot is
 * loaded we need them to be first. The rest of the files are sorted alphabetically.
 *
 * The `filePaths` list is of the form [["folder", "file.so"], ["file.so"]], so each element in it
 * is effectively a file path.
 */
function sortSoFiles(filePaths: FilePath[]): FilePath[] {
  let result = [];
  let hasLzma = false;
  let hasSsl = false;
  const lzmaFile = '_lzma.so';
  const sslFile = '_ssl.so';
  for (const path of filePaths) {
    if (path.length == 1 && path[0] == lzmaFile) {
      hasLzma = true;
    } else if (path.length == 1 && path[0] == sslFile) {
      hasSsl = true;
    } else {
      result.push(path);
    }
  }

  // JS might handle sorting lists of lists fine, but I'd rather be explicit here and make it compare
  // strings.
  result = result
    .map((x) => x.join('/'))
    .sort()
    .map((x) => x.split('/'));
  if (hasSsl) {
    result.unshift([sslFile]);
  }
  if (hasLzma) {
    result.unshift([lzmaFile]);
  }

  return result;
}

/**
 * Used to ensure that the memoryBase of the dynamic library is stable when restoring snapshots.
 * If the dynamic library was loaded in a memory snapshot,
 */
function getMemoryPatched(
  Module: Module,
  libPath: string,
  size: number
): number {
  if (Module.API.version === '0.26.0a2') {
    return Module.getMemory(size);
  }
  // Sometimes the module is loaded once by path and once by name, in either order. I'm not really
  // sure why. But let's check if we snapshoted a load of the library by name.
  const libName = libPath.split('/').at(-1)!;
  // 1. Is it loaded in the snapshot? Replay the memory base.
  {
    const { soMemoryBases } = LOADED_SNAPSHOT_META ?? {};
    // If we loaded this library before taking the snapshot, we already allocated the memory and the
    // allocator remembers because its state is in the linear memory. We just have to look it up.
    const base = soMemoryBases?.[libPath] ?? soMemoryBases?.[libName];
    if (base) {
      return base;
    }
  }
  // 2. It's not loaded in the snapshot. Record
  {
    const { loadOrder, soMemoryBases, soTableBases } = CREATED_SNAPSHOT_META;
    // Okay, we didn't load this before so we need to allocate new memory for it. Also record what we
    // did in case someone makes a snapshot from this run.
    loadOrder.push(libPath);
    const memoryBase = Module.getMemory(size);
    // Track both by full path and by name. That gives us a chance to resolve conflicts in name by the
    // full path.
    soMemoryBases[libPath] = memoryBase;
    soMemoryBases[libName] = memoryBase;
    soTableBases[libPath] = Module.wasmTable.length;
    soTableBases[libName] = Module.wasmTable.length;
    return memoryBase;
  }
}

function loadDynlibFromTarFs(
  Module: Module,
  base: string,
  node: TarFSInfo | undefined,
  soFile: string[]
): void {
  for (const part of soFile) {
    node = node?.children?.get(part);
  }
  if (!node?.contentsOffset) {
    node = VIRTUALIZED_DIR.getDynlibRoot();
    for (const part of soFile) {
      node = node?.children?.get(part);
    }
  }
  if (!node?.contentsOffset) {
    throw Error(`fs node could not be found for ${soFile.join('/')}`);
  }
  const { contentsOffset, size } = node;
  if (contentsOffset === undefined) {
    throw Error(`contentsOffset not defined for ${soFile.join('/')}`);
  }
  const wasmModuleData = new Uint8Array(size);
  (node.reader ?? EmbeddedPackagesTarReader).read(
    contentsOffset,
    wasmModuleData
  );
  const path = base + soFile.join('/');
  loadDynlib(Module, path, wasmModuleData);
}

function loadDynlibFromVendor(
  Module: Module,
  soFile: string[],
  userBundleNames: string[]
): void {
  const path = soFile.slice(3).join('/');
  const index = userBundleNames.indexOf(path);
  if (index == -1) {
    throw new PythonRuntimeError(
      `Could not find ${path} in user bundle, which is required by the snapshot.`
    );
  }
  // The MetadataReader holds the user bundle's contents.
  const buffer = new Uint8Array(MetadataReader.getSizes()[index]!);
  MetadataReader.read(index, 0, buffer);
  loadDynlib(Module, path, buffer);
}

/**
 * Preloading for the legacy 0.26 version. This loads all dynamic libraries
 * visible in the site-packages directory. They are loaded before the runtime is
 * initialized outside of the heap, using the same mechanism for DT_NEEDED libs
 * (i.e., the libs that are loaded before the program starts because you passed
 * them as linker args).
 */
function preloadDynamicLibs026(Module: Module): void {
  const sitePackages = Module.FS.sessionSitePackages + '/';
  const sitePackagesRoot = VIRTUALIZED_DIR.getSitePackagesRoot();
  const loadedBaselineSnapshot =
    LOADED_SNAPSHOT_META?.settings?.baselineSnapshot;
  let SO_FILES_TO_LOAD: string[][];
  if (IS_CREATING_BASELINE_SNAPSHOT || loadedBaselineSnapshot) {
    SO_FILES_TO_LOAD = [['_lzma.so'], ['_ssl.so']];
  } else {
    SO_FILES_TO_LOAD = sortSoFiles(VIRTUALIZED_DIR.getSoFilesToLoad());
  }
  for (const soFile of SO_FILES_TO_LOAD) {
    loadDynlibFromTarFs(Module, sitePackages, sitePackagesRoot, soFile);
  }
}

/**
 * If we're restoring from a snapshot, we need to preload dynamic libraries so that any function
 * pointers that point into the dylib symbols work correctly.
 * Load the dynamic libraries in loadOrder. Mostly logic dealing with paths.
 */
function preloadDynamicLibsMain(Module: Module, loadOrder: string[]): void {
  const sitePackages = Module.FS.sessionSitePackages + '/';
  const sitePackagesRoot = VIRTUALIZED_DIR.getSitePackagesRoot();
  const dynlibRoot = VIRTUALIZED_DIR.getDynlibRoot();
  const dynlibPath = '/usr/lib/';
  const userBundleNames = MetadataReader.getNames();
  for (let path of loadOrder) {
    let root = sitePackagesRoot;
    let base = '';
    if (path.startsWith(sitePackages)) {
      path = path.slice(sitePackages.length);
      base = sitePackages;
    } else if (path.startsWith(dynlibPath)) {
      path = path.slice(dynlibPath.length);
      root = dynlibRoot;
      base = dynlibPath;
    }

    const pathSplit = path.split('/');
    if (pathSplit[0] == '') {
      // This is a file path beginning with `/`, like /session/metadata/vendor/pkg/lib.so. So we
      // are loading the vendored package's dynlibs here.
      //
      // TODO(EW-9508): support .so's in user bundle outside vendor dir.
      loadDynlibFromVendor(Module, pathSplit, userBundleNames);
    } else {
      // This is a file path relative to the site-packages directory, like pkg/lib.so. So we are
      // loading the built-in package's dynlibs here.
      loadDynlibFromTarFs(Module, base, root, pathSplit);
    }
  }
}

function preloadDynamicLibs(Module: Module): void {
  if (Module.API.version === '0.26.0a2') {
    // In 0.26.0a2 we need to preload dynamic libraries even if we aren't restoring a snapshot.
    preloadDynamicLibs026(Module);
    return;
  }
  //
  const loadOrder = LOADED_SNAPSHOT_META?.loadOrder;
  if (!loadOrder) {
    // In newer versions we only need to do the preloading if there is a snapshot to restore.
    return;
  }
  // In Pyodide 0.28 we switched from using top level EM_JS to initialize the CountArgs function
  // pointer to using an initializer to work around a regression in Emscripten 4.0.3 and 4.0.4. We
  // could drop this patch because we are now on Emscripten 4.0.9.
  // https://github.com/pyodide/pyodide/blob/main/cpython/patches/0008-Fix-Emscripten-call-trampoline-compatibility-with-Em.patch
  //
  // Unfortunately, this initializer allocates a function table slot and is called before dynamic
  // loading when taking the snapshot but after when restoring the snapshot. Thus, when restoring a
  // snapshot, before loading dynamic libraries we reserve a function pointer for the
  // CountArgsPointer and after loading dynamic libraries, we put it in the free list so it will be
  // used at the right moment.
  const PyEMCountArgsPtr = Module.getEmptyTableSlot();
  preloadDynamicLibsMain(Module, loadOrder);
  Module.freeTableIndexes.push(PyEMCountArgsPtr);
}

/**
 * This records which dynamic libraries have open handles (handed out by dlopen,
 * not yet dlclosed). We'll need to track this information so that we don't
 * crash if we dlsym the handle after restoring from the snapshot
 */
function recordDsoHandles(Module: Module): DsoHandles {
  const dylinkInfo: DsoHandles = {};
  for (const [h, { name }] of Object.entries(Module.LDSO.loadedLibsByHandle)) {
    const handle = Number(h);
    if (handle === 0) {
      continue;
    }
    dylinkInfo[name] ??= { handles: [] };
    dylinkInfo[name].handles.push(handle);
  }
  return dylinkInfo;
}

/**
 * Python modules do a lot of work the first time they are imported. The memory
 * snapshot will save more time the more of this work is included. However, we
 * can't snapshot the JS runtime state so we have no ffi. Thus some imports from
 * user code will fail.
 *
 * If we are doing a baseline snapshot, just import everything from
 * baselineSnapshotImports. These will all succeed.
 *
 * If doing a more dedicated "package" snap shot, also try to import each
 * user import that is importing non-vendored modules.
 *
 * All of this is being done in the __main__ global scope, so be careful not to
 * pollute it with extra included-by-default names (user code is executed in its
 * own separate module scope though so it's not _that_ important).
 *
 * This function returns a list of modules that have been imported.
 */
function memorySnapshotDoImports(Module: Module): string[] {
  const baselineSnapshotImports =
    MetadataReader.constructor.getBaselineSnapshotImports();
  const toImport = baselineSnapshotImports.join(',');
  const toDelete = Array.from(
    new Set(baselineSnapshotImports.map((x) => x.split('.', 1)[0]))
  ).join(',');

  simpleRunPython(Module, `import ${toImport}`);
  simpleRunPython(Module, 'sysconfig.get_config_vars()');
  // Delete to avoid polluting globals
  simpleRunPython(Module, `del ${toDelete}`);
  if (IS_CREATING_BASELINE_SNAPSHOT) {
    // We've done all the imports for the baseline snapshot.
    return [];
  }

  if (REQUIREMENTS.length == 0) {
    // Don't attempt to scan for package imports if the Worker has specified no package
    // requirements, as this means their code isn't going to be importing any modules that we need
    // to include in a snapshot.
    return [];
  }

  // The `importedModules` list will contain all modules that have been imported, including local
  // modules, the usual `js` and other stdlib modules. We want to filter out local imports, so we
  // grab them and put them into a set for fast filtering.
  const importedModules: string[] = MetadataReader.getPackageSnapshotImports(
    Module.API.version
  );
  const deduplicatedModules = [...new Set(importedModules)];

  // Import the modules list so they are included in the snapshot.
  if (deduplicatedModules.length > 0) {
    simpleRunPython(Module, 'import ' + deduplicatedModules.join(','));
  }

  return deduplicatedModules;
}

function describeValue(val: any): string {
  try {
    const out = [];

    const type = typeof val;
    const isObject = type === 'object';
    out.push(`Value: ${val}`);
    out.push(`Type: ${type}`);

    if (val && isObject) {
      try {
        out.push(
          `Keys: ${Object.keys(val as Record<string, unknown>)
            .slice(0, 10)
            .join(', ')}`
        );
      } catch {
        // Ignore errors when getting keys
      }
    }

    try {
      if (val && isObject && typeof (val as Error).stack === 'string') {
        out.push(`Stack:\n${(val as Error).stack}`);
      }
    } catch {
      // Ignore errors when getting stack
    }

    try {
      out.push(`Prototype: ${Object.prototype.toString.call(val)}`);
    } catch {
      // Ignore errors when getting object type
    }

    try {
      const contents = JSON.stringify(val);
      if (contents?.length > 100) {
        out.push(`Contents: ${contents.slice(0, 100)}...`);
      } else if (contents) {
        out.push(`Contents: ${contents}`);
      }
    } catch {
      // Ignore JSON stringify errors
    }

    return out.join('\n');
  } catch (err) {
    return `Error describing value: ${err}`;
  }
}

/**
 * When we create a dedicated memory snapshot, we capture all the globals in the user's top-level
 * scope. If those globals refer to JS objects, then we may fail to serialise them. This function
 * creates a user error to inform the user of this issue.
 *
 * It's important that we give the user as much information about this as possible, so they can
 * understand what they need to change in order to resolve the problem on their end.
 */
function createUnserializableObjectError(obj: any): PythonUserError {
  // TODO: Create docs for this and link them here.
  const error = `Can't serialize top-level variable.
Please review any global variables you or your imported modules create at the top-level of your code, and consider deleting them.

Description of the value:
${describeValue(obj)}
`;
  return new PythonUserError(error);
}

/**
 * Create memory snapshot by importing SNAPSHOT_IMPORTS to ensure these packages
 * are initialized in the linear memory snapshot and then saving a copy of the
 * linear memory into MEMORY.
 */
function makeLinearMemorySnapshot(
  Module: Module,
  importedModulesList: string[],
  pyodide_entrypoint_helper: PyodideEntrypointHelper | null,
  snapshotType: ArtifactBundler.SnapshotType
): Uint8Array {
  const customHiwireStateSerializer = (obj: any): Record<string, boolean> => {
    if (obj === pyodide_entrypoint_helper) {
      return { pyodide_entrypoint_helper: true };
    }
    throw createUnserializableObjectError(obj);
  };

  const dsoHandles = recordDsoHandles(Module);
  const hiwire =
    Module.API.version === '0.26.0a2'
      ? undefined
      : Module.API.serializeHiwireState(customHiwireStateSerializer);
  const settings: SnapshotSettings = {
    baselineSnapshot: IS_CREATING_BASELINE_SNAPSHOT,
    snapshotType,
    compatFlags: COMPATIBILITY_FLAGS,
  };
  return encodeSnapshot(Module.HEAP8, {
    version: 1,
    dsoHandles,
    hiwire,
    importedModulesList,
    settings,
    ...CREATED_SNAPSHOT_META,
  });
}

/**
 * Encode heap and dsoJSON into the memory snapshot artifact that we'll upload
 */
function encodeSnapshot(heap: Uint8Array, meta: SnapshotMeta): Uint8Array {
  const json = JSON.stringify(meta);
  let snapshotOffset = HEADER_SIZE + 2 * json.length;
  // align to 8 bytes
  snapshotOffset = Math.ceil(snapshotOffset / 8) * 8;
  const toUpload = new Uint8Array(snapshotOffset + heap.length);
  const encoder = new TextEncoder();
  const { written: jsonByteLength } = encoder.encodeInto(
    json,
    toUpload.subarray(HEADER_SIZE)
  );
  const header = new Uint32Array(toUpload.buffer);
  header[0] = SNAPSHOT_MAGIC;
  header[1] = CREATE_SNAPSHOT_VERSION;
  header[2] = snapshotOffset;
  header[3] = jsonByteLength;
  toUpload.subarray(snapshotOffset).set(heap);
  return toUpload;
}

/**
 * Decode heap and dsoJSON from the memory snapshot reader
 */
function decodeSnapshot(
  reader: SnapshotReader | undefined
): LoadedSnapshotMeta | undefined {
  if (!reader) {
    return undefined;
  }
  if (reader.getMemorySnapshotSize() === 0) {
    throw new PythonRuntimeError(
      `SnapshotReader returned memory snapshot size of 0`
    );
  }
  const header = new Uint32Array(4);
  reader.readMemorySnapshot(0, header);
  if (header[0] !== SNAPSHOT_MAGIC) {
    throw new PythonRuntimeError(
      `Invalid magic number ${header[0]}, expected ${SNAPSHOT_MAGIC}`
    );
  }
  // buf[1] is SNAPSHOT_VERSION (unused currently)
  const snapshotOffset = header[2]!;
  const jsonByteLength = header[3]!;

  const snapshotSize = reader.getMemorySnapshotSize() - snapshotOffset;
  const jsonBuf = new Uint8Array(jsonByteLength);
  const offset = header.byteLength; // the json starts after the header
  reader.readMemorySnapshot(offset, jsonBuf);
  const json = new TextDecoder().decode(jsonBuf);
  const meta = JSON.parse(json) as OldSnapshotMeta | SnapshotMeta;
  const extras: LoadedSnapshotExtras = {
    snapshotSize,
    snapshotOffset,
    snapshotReader: reader,
  };
  if (!meta?.version) {
    return {
      version: 1,
      importedModulesList: undefined,
      dsoHandles: meta,
      hiwire: undefined,
      loadOrder: [],
      soMemoryBases: {},
      settings: {
        snapshotType: meta.settings?.baselineSnapshot ? 'baseline' : 'package',
        compatFlags: {},
        ...meta.settings,
      },
      ...extras,
    };
  }
  return {
    ...meta,
    ...extras,
    settings: {
      ...meta.settings,
      snapshotType:
        meta.settings.snapshotType ??
        (meta.settings.baselineSnapshot ? 'baseline' : 'package'),
      compatFlags: meta.settings.compatFlags ?? {},
    },
  };
}

export function isRestoringSnapshot(): boolean {
  return !!LOADED_SNAPSHOT_META;
}

function checkSnapshotType(snapshotType: string): void {
  if (SHOULD_SNAPSHOT_TO_DISK) {
    return;
  }
  if (
    !IS_EW_VALIDATING &&
    snapshotType === 'dedicated' &&
    !IS_DEDICATED_SNAPSHOT_ENABLED
  ) {
    throw new PythonRuntimeError(
      'Received dedicated snapshot but compat flag for dedicated snapshots is not enabled'
    );
  }

  if (
    !IS_EW_VALIDATING &&
    snapshotType !== 'dedicated' &&
    IS_DEDICATED_SNAPSHOT_ENABLED
  ) {
    throw new PythonRuntimeError(
      'Received non-dedicated snapshot but compat flag for dedicated snapshots is enabled'
    );
  }

  // If we have a snapshot in the bundle and the dedicated snapshot flag is enabled, then we
  // should verify that the snapshot in the bundle is a dedicated snapshot. If it is not
  // we should fail with an error.
  if (snapshotType !== 'dedicated' && IS_SECOND_VALIDATION_PHASE) {
    throw new PythonRuntimeError(
      'The second validation phase should receive a dedicated snapshot, got ' +
        snapshotType
    );
  }
}

export function maybeRestoreSnapshot(Module: Module): void {
  Module.noInitialRun = isRestoringSnapshot();
  Module.getMemoryPatched = getMemoryPatched;
  // Make sure memory is large enough
  Module.growMemory(LOADED_SNAPSHOT_META?.snapshotSize ?? 0);
  enterJaegerSpan('preload_dynamic_libs', () => {
    preloadDynamicLibs(Module);
  });
  // TODO: Remove the jaeger span here
  enterJaegerSpan('remove_run_dependency', () => {
    Module.removeRunDependency('dynlibs');
  });
  if (!LOADED_SNAPSHOT_META) {
    return;
  }
  const { snapshotSize, snapshotOffset, snapshotReader, settings } =
    LOADED_SNAPSHOT_META;
  checkSnapshotType(settings.snapshotType);

  Module.growMemory(snapshotSize);
  snapshotReader.readMemorySnapshot(snapshotOffset, Module.HEAP8);
  snapshotReader.disposeMemorySnapshot();
  // Invalidate caches if we have a snapshot because the contents of site-packages
  // may have changed.
  invalidateCaches(Module);
}

function collectSnapshot(
  Module: Module,
  importedModulesList: string[],
  pyodide_entrypoint_helper: PyodideEntrypointHelper | null,
  snapshotType: ArtifactBundler.SnapshotType
): void {
  if (IS_EW_VALIDATING) {
    const snapshot = makeLinearMemorySnapshot(
      Module,
      importedModulesList,
      pyodide_entrypoint_helper,
      snapshotType
    );
    ArtifactBundler.storeMemorySnapshot({
      snapshot,
      importedModulesList,
      snapshotType,
    });
  } else if (SHOULD_SNAPSHOT_TO_DISK) {
    const snapshot = makeLinearMemorySnapshot(
      Module,
      importedModulesList,
      pyodide_entrypoint_helper,
      snapshotType
    );
    DiskCache.put('snapshot.bin', snapshot);
  } else {
    throw new PythonRuntimeError(
      "Attempted to collect snapshot outside of context where it's supported."
    );
  }
}

/**
 * Collects a dedicated snapshot. This is only called after the top-level of the worker has been
 * run.
 */
export function maybeCollectDedicatedSnapshot(
  Module: Module,
  pyodide_entrypoint_helper: PyodideEntrypointHelper | null
): void {
  if (!IS_CREATING_SNAPSHOT) {
    return;
  }

  if (!IS_DEDICATED_SNAPSHOT_ENABLED) {
    return;
  }

  if (Module.API.version == '0.26.0a2') {
    // 0.26.0a2 does not support serialisation of the hiwire state, so it cannot support dedicated
    // snapshots.
    throw new PythonRuntimeError(
      'Dedicated snapshot is not supported for Python runtime version 0.26.0a2'
    );
  }

  if (!pyodide_entrypoint_helper) {
    throw new PythonRuntimeError(
      'pyodide_entrypoint_helper is required for dedicated snapshot'
    );
  }
  collectSnapshot(Module, [], pyodide_entrypoint_helper, 'dedicated');
}

/**
 * Collects either a baseline or package snapshot. This is called prior to running the top-level
 * of the worker and crucially before the worker files are mounted.
 *
 * Dedicated snapshots are collected in `maybeCollectDedicatedSnapshot`.
 */
export function maybeCollectSnapshot(Module: Module): void {
  // In order to surface any problems that occur in `memorySnapshotDoImports` to
  // users in local development, always call it even if we aren't actually
  const importedModulesList = memorySnapshotDoImports(Module);
  if (!IS_CREATING_SNAPSHOT) {
    return;
  }

  if (IS_DEDICATED_SNAPSHOT_ENABLED) {
    // We are not interested in collecting a baseline/package snapshot here if this feature flag
    // is enabled.
    return;
  }

  collectSnapshot(
    Module,
    importedModulesList,
    null,
    IS_CREATING_BASELINE_SNAPSHOT ? 'baseline' : 'package'
  );
}

export function finalizeBootstrap(
  Module: Module,
  pyodide_entrypoint_helper: PyodideEntrypointHelper
): void {
  const customHiwireStateDeserializer = (obj: any): any => {
    if ('pyodide_entrypoint_helper' in obj) {
      return pyodide_entrypoint_helper;
    }
    throw new PythonRuntimeError(`Can't deserialize ${obj}`);
  };

  Module.API.config._makeSnapshot =
    IS_CREATING_SNAPSHOT && Module.API.version !== '0.26.0a2';
  enterJaegerSpan('finalize_bootstrap', () => {
    Module.API.finalizeBootstrap(
      LOADED_SNAPSHOT_META?.hiwire,
      customHiwireStateDeserializer
    );
  });
  // finalizeBootstrap overrides LD_LIBRARY_PATH. Restore it.
  simpleRunPython(
    Module,
    `import os; os.environ["LD_LIBRARY_PATH"] += ":/session/metadata/python_modules/lib/"; del os`
  );
  if (IS_CREATING_SNAPSHOT) {
    return;
  }
  Module.API.public_api.registerJsModule('_cf_internal_snapshot_info', {
    loadedSnapshot: !!LOADED_SNAPSHOT_META,
    loadedBaselineSnapshot: LOADED_SNAPSHOT_META?.settings.baselineSnapshot,
    importedModulesList: LOADED_SNAPSHOT_META?.importedModulesList,
  });
}
