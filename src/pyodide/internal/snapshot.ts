import { enterJaegerSpan } from 'pyodide-internal:jaeger';
import { default as ArtifactBundler } from 'pyodide-internal:artifacts';
import { default as UnsafeEval } from 'internal:unsafe-eval';
import { default as DiskCache } from 'pyodide-internal:disk_cache';
import { FilePath, VIRTUALIZED_DIR } from 'pyodide-internal:setupPackages';
import { default as EmbeddedPackagesTarReader } from 'pyodide-internal:packages_tar_reader';
import {
  SHOULD_SNAPSHOT_TO_DISK,
  IS_CREATING_BASELINE_SNAPSHOT,
  MEMORY_SNAPSHOT_READER,
  REQUIREMENTS,
  IS_CREATING_SNAPSHOT,
  IS_EW_VALIDATING,
} from 'pyodide-internal:metadata';
import { simpleRunPython } from 'pyodide-internal:util';
import { default as MetadataReader } from 'pyodide-internal:runtime-generated/metadata';

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

// The new wire format, with additional information about the hiwire state, the order that dsos were
// loaded in, and their memory bases. We also moved settings out of the dsoHandles.
type SnapshotMeta = {
  readonly hiwire: SnapshotConfig | undefined;
  readonly dsoHandles: DsoHandles;
  readonly settings: { readonly baselineSnapshot: boolean };
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

type LoadedSnapshotMeta = SnapshotMeta & LoadedSnapshotExtras;

/**
 * Global variables for the memory snapshot.
 */
let LOADED_SNAPSHOT_META: LoadedSnapshotMeta | undefined;
const CREATED_SNAPSHOT_META: DsoLoadInfo = {
  soMemoryBases: {},
  loadOrder: [],
};

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
    const { loadOrder, soMemoryBases } = CREATED_SNAPSHOT_META;
    // Okay, we didn't load this before so we need to allocate new memory for it. Also record what we
    // did in case someone makes a snapshot from this run.
    loadOrder.push(libPath);
    const memoryBase = Module.getMemory(size);
    // Track both by full path and by name. That gives us a chance to resolve conflicts in name by the
    // full path.
    soMemoryBases[libPath] = memoryBase;
    soMemoryBases[libName] = memoryBase;
    return memoryBase;
  }
}

/**
 * This loads all dynamic libraries visible in the site-packages directory. They
 * are loaded before the runtime is initialized outside of the heap, using the
 * same mechanism for DT_NEEDED libs (i.e., the libs that are loaded before the
 * program starts because you passed them as linker args).
 *
 * Currently, we pessimistically preload all libs. It would be nice to only load
 * the ones that are used. I am pretty sure we can manage this by reserving a
 * separate shared lib metadata arena at startup and allocating shared libs
 * there.
 */
export function preloadDynamicLibs(Module: Module): void {
  Module.noInitialRun = isRestoringSnapshot();
  Module.getMemoryPatched = getMemoryPatched;
  Module.growMemory(LOADED_SNAPSHOT_META?.snapshotSize ?? 0);
  let SO_FILES_TO_LOAD: string[][] = [];
  const sitePackages = Module.FS.sessionSitePackages + '/';
  if (Module.API.version === '0.26.0a2') {
    const loadedBaselineSnapshot =
      LOADED_SNAPSHOT_META?.settings?.baselineSnapshot;
    if (IS_CREATING_BASELINE_SNAPSHOT || loadedBaselineSnapshot) {
      SO_FILES_TO_LOAD = [['_lzma.so'], ['_ssl.so']];
    } else {
      SO_FILES_TO_LOAD = sortSoFiles(VIRTUALIZED_DIR.getSoFilesToLoad());
    }
  } else if (LOADED_SNAPSHOT_META?.loadOrder) {
    SO_FILES_TO_LOAD = LOADED_SNAPSHOT_META.loadOrder.map((x) => {
      // We need the path relative to the site-packages directory, not relative to the root of the file
      // system.
      if (x.startsWith(sitePackages)) {
        x = x.slice(sitePackages.length);
      }
      return x.split('/');
    });
  }

  for (const soFile of SO_FILES_TO_LOAD) {
    let node: TarFSInfo | undefined = VIRTUALIZED_DIR.getSitePackagesRoot();
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
    const path = sitePackages + soFile.join('/');
    loadDynlib(Module, path, wasmModuleData);
  }
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
    if (!(name in dylinkInfo)) {
      dylinkInfo[name] = { handles: [] };
    }
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

/**
 * Create memory snapshot by importing SNAPSHOT_IMPORTS to ensure these packages
 * are initialized in the linear memory snapshot and then saving a copy of the
 * linear memory into MEMORY.
 */
function makeLinearMemorySnapshot(Module: Module): Uint8Array {
  const dsoHandles = recordDsoHandles(Module);
  const hiwire =
    Module.API.version === '0.26.0a2'
      ? undefined
      : Module.API.serializeHiwireState(Module);
  const settings = {
    baselineSnapshot: IS_CREATING_BASELINE_SNAPSHOT,
  };
  return encodeSnapshot(Module.HEAP8, {
    version: 1,
    dsoHandles,
    hiwire,
    settings,
    ...CREATED_SNAPSHOT_META,
  });
}

// "\x00snp"
const SNAPSHOT_MAGIC = 0x706e7300;
const CREATE_SNAPSHOT_VERSION = 2;
const HEADER_SIZE = 4 * 4;

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

let TEST_SNAPSHOT: Uint8Array | undefined = undefined;
(function (): void {
  // Lookup memory snapshot from artifact store.
  if (!MEMORY_SNAPSHOT_READER) {
    // snapshots are disabled or there isn't one yet
    return;
  }

  // Simple sanity check to ensure this snapshot isn't corrupted.
  //
  // TODO(later): we need better detection when this is corrupted. Right now the isolate will
  // just die.
  const snapshotSize = MEMORY_SNAPSHOT_READER.getMemorySnapshotSize();
  if (snapshotSize <= 100) {
    TEST_SNAPSHOT = new Uint8Array(snapshotSize);
    MEMORY_SNAPSHOT_READER.readMemorySnapshot(0, TEST_SNAPSHOT);
    return;
  }
  LOADED_SNAPSHOT_META = decodeSnapshot(MEMORY_SNAPSHOT_READER);
})();

/**
 * Decode heap and dsoJSON from the memory snapshot reader
 */
function decodeSnapshot(reader: SnapshotReader): LoadedSnapshotMeta {
  const header = new Uint32Array(4);
  reader.readMemorySnapshot(0, header);
  if (header[0] !== SNAPSHOT_MAGIC) {
    throw new Error(
      `Invalid magic number ${header[0]}, expected ${SNAPSHOT_MAGIC}`
    );
  }
  // buf[1] is SNAPSHOT_VERSION (unused currently)
  const snapshotOffset = header[2];
  const jsonByteLength = header[3];

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
      dsoHandles: meta,
      hiwire: undefined,
      loadOrder: [],
      soMemoryBases: {},
      settings: { baselineSnapshot: false, ...meta.settings },
      ...extras,
    };
  }
  return { ...meta, ...extras };
}

export function isRestoringSnapshot(): boolean {
  return !!LOADED_SNAPSHOT_META;
}

export function maybeRestoreSnapshot(Module: Module): void {
  if (!LOADED_SNAPSHOT_META) {
    return;
  }
  const { snapshotSize, snapshotOffset, snapshotReader } = LOADED_SNAPSHOT_META;
  Module.growMemory(snapshotSize);
  snapshotReader.readMemorySnapshot(snapshotOffset, Module.HEAP8);
  snapshotReader.disposeMemorySnapshot();
  // Invalidate caches if we have a snapshot because the contents of site-packages
  // may have changed.
  simpleRunPython(
    Module,
    'from importlib import invalidate_caches as f; f(); del f'
  );
}

export function finishSnapshotSetup(pyodide: Pyodide): void {
  // This is just here for our test suite. Ugly but just about the only way to test this.
  if (TEST_SNAPSHOT) {
    const snapshotString = new TextDecoder().decode(TEST_SNAPSHOT);
    pyodide.registerJsModule('cf_internal_test_utils', {
      snapshot: snapshotString,
    });
  }
}

export function maybeCollectSnapshot(Module: Module): void {
  // In order to surface any problems that occur in `memorySnapshotDoImports` to
  // users in local development, always call it even if we aren't actually
  const importedModulesList = memorySnapshotDoImports(Module);
  if (!IS_CREATING_SNAPSHOT) {
    return;
  }
  if (IS_EW_VALIDATING) {
    const snapshot = makeLinearMemorySnapshot(Module);
    ArtifactBundler.storeMemorySnapshot({ snapshot, importedModulesList });
  } else if (SHOULD_SNAPSHOT_TO_DISK) {
    const snapshot = makeLinearMemorySnapshot(Module);
    // TODO(soon): Get rid of this type coercion.
    DiskCache.put('snapshot.bin', snapshot as unknown as ArrayBuffer);
  }
}

export function finalizeBootstrap(Module: Module): void {
  Module.API.config._makeSnapshot =
    IS_CREATING_SNAPSHOT && Module.API.version !== '0.26.0a2';
  enterJaegerSpan('finalize_bootstrap', () => {
    Module.API.finalizeBootstrap(LOADED_SNAPSHOT_META?.hiwire);
  });
}
