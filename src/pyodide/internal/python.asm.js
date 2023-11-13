
var Module = (() => {
  var _scriptDir = import.meta.url;
  
  return (
async function(moduleArg = {}) {

// include: shell.js
// The Module object: Our interface to the outside world. We import
// and export values on it. There are various ways Module can be used:
// 1. Not defined. We create it here
// 2. A function parameter, function(Module) { ..generated code.. }
// 3. pre-run appended it, var Module = {}; ..generated code..
// 4. External script tag defines var Module.
// We need to check if Module already exists (e.g. case 3 above).
// Substitution will be replaced with actual code on later stage of the build,
// this way Closure Compiler will not mangle it (e.g. case 4. above).
// Note that if you want to run closure, and also to use Module
// after the generated code, you will need to define   var Module = {};
// before the code. Then that object will be used in the code, and you
// can continue to use Module afterwards as well.
var Module = moduleArg;

// Set up the promise that indicates the Module is initialized
var readyPromiseResolve, readyPromiseReject;
Module['ready'] = new Promise((resolve, reject) => {
  readyPromiseResolve = resolve;
  readyPromiseReject = reject;
});
["_main","_free","_hiwire_new","_hiwire_intern","_hiwire_num_refs","_hiwire_get","_hiwire_incref","_hiwire_decref","_hiwire_pop","_PyBuffer_Release","_Py_DecRef","_PyDict_New","_PyDict_SetItem","__PyErr_CheckSignals","_PyErr_CheckSignals","_PyErr_Clear","_PyErr_Occurred","_PyErr_Print","_PyErr_SetString","_PyEval_SaveThread","_PyEval_RestoreThread","_PyFloat_FromDouble","_PyGILState_Check","_Py_IncRef","_PyList_New","_PyList_SetItem","__PyLong_FromByteArray","_PyLong_FromDouble","_PyMem_Free","_PyObject_GetAIter","_PyObject_GetIter","_PyObject_Size","_PyRun_SimpleString","_PySet_Add","_PySet_New","__PyTraceback_Add","_PyUnicode_Data","_PyUnicode_New","_memory","_descr_set_trampoline_call","_descr_get_trampoline_call","__PyCFunctionWithKeywords_TrampolineCall","__PyImport_InitFunc_TrampolineCall","__Py_emscripten_runtime","_Py_EMSCRIPTEN_SIGNAL_HANDLING","__Py_CheckEmscriptenSignals_Helper","_dump_traceback","_set_error","_restore_sys_last_exception","_wrap_exception","_pythonexc2js","_console_error","_console_error_obj","_new_error","_fail_test","_JsString_FromId","_hiwire_init_js","_hiwire_invalid_ref","_Js_true","_Js_false","_hiwire_to_bool","_hiwire_int","_hiwire_int_from_digits","_hiwire_double","_hiwire_string_utf8","_hiwire_throw_error","_hiwire_call","_hiwire_call_OneArg","_hiwire_call_bound","_hiwire_HasMethod","_hiwire_CallMethodString","_hiwire_CallMethod","_hiwire_CallMethod_NoArgs","_hiwire_CallMethod_OneArg","_hiwire_construct","_hiwire_has_length","_hiwire_get_length_helper","_hiwire_get_length_string","_hiwire_get_bool","_hiwire_is_function","_hiwire_is_generator","_hiwire_is_async_generator","_hiwire_is_comlink_proxy","_hiwire_is_error","_hiwire_is_promise","_hiwire_resolve_promise","_hiwire_to_string","_hiwire_typeof","_hiwire_constructor_name","_hiwire_less_than","_hiwire_less_than_equal","_hiwire_equal","_hiwire_not_equal","_hiwire_greater_than","_hiwire_greater_than_equal","_hiwire_reversed_iterator","_hiwire_assign_to_ptr","_hiwire_assign_from_ptr","_hiwire_read_from_file","_hiwire_write_to_file","_hiwire_into_file","_hiwire_get_buffer_info","_hiwire_subarray","_JsArray_Check","_JsArray_New","_JsArray_Push","_JsArray_Push_unchecked","_JsArray_Extend","_JsArray_Get","_JsArray_Set","_JsArray_Delete","_JsArray_Splice","_JsArray_slice","_JsArray_slice_assign","_JsArray_Clear","_JsArray_ShallowCopy","_JsObject_New","_isReservedWord","_normalizeReservedWords","_JsObject_GetString","_JsObject_SetString","_JsObject_DeleteString","_JsObject_Dir","_JsObject_Entries","_JsObject_Keys","_JsObject_Values","_JsString_InternFromCString","_JsMap_New","_JsMap_Set","_JsSet_New","_JsSet_Add","_Js_undefined","_Js_null","___em_lib_deps_pyodide_core_deps","__js2python_none","__js2python_true","__js2python_false","__js2python_pyproxy","_js2python_immutable","_js2python","_js2python_immutable_js","_js2python_js","_js2python_convert","_js2python_init","__agen_handle_result_js_c","_JsBuffer_CopyIntoMemoryView","_JsProxy_create","_JsProxy_Check","_JsProxy_GetIter_js","_handle_next_result_js","_JsException_new_helper","_JsProxy_GetAsyncIter_js","__agen_handle_result_js","_destroy_jsarray_entries","_JsArray_repeat_js","_JsArray_inplace_repeat_js","_JsArray_index_helper","_JsArray_count_helper","_JsArray_reverse_helper","_JsProxy_subscript_js","_JsMap_GetIter_js","_JsMap_clear_js","_JsObjMap_GetIter_js","_JsObjMap_length_js","_JsObjMap_subscript_js","_JsObjMap_ass_subscript_js","_JsObjMap_contains_js","_get_async_js_call_done_callback","_wrap_generator","_wrap_async_generator","_JsBuffer_DecodeString_js","_JsDoubleProxy_unwrap_helper","_JsProxy_compute_typeflags","_check_gil","_pyproxy_getflags","__pyproxy_repr","__pyproxy_type","__pyproxy_hasattr","__pyproxy_getattr","__pyproxy_setattr","__pyproxy_delattr","__pyproxy_getitem","__pyproxy_setitem","__pyproxy_delitem","__pyproxy_slice_assign","__pyproxy_pop","__pyproxy_contains","__pyproxy_ownKeys","__pyproxy_apply","__iscoroutinefunction","__pyproxy_iter_next","__pyproxyGen_Send","__pyproxyGen_return","__pyproxyGen_throw","__pyproxyGen_asend","__pyproxyGen_areturn","__pyproxyGen_athrow","__pyproxy_aiter_next","__pyproxy_ensure_future","__pyproxy_get_buffer","_throw_no_gil","_pyproxy_Check","_pyproxy_AsPyObject","_destroy_proxies","_gc_register_proxies","_destroy_proxy","_proxy_cache_get","_proxy_cache_set","_array_to_js","_buffer_struct_size","_pyproxy_new_ex","_pyproxy_new","_create_once_callable","_create_promise_handles","__python2js_buffer_inner","_python2js_buffer_init","__python2js","_python2js","_python2js_with_depth","_python2js_custom","__python2js_addto_postprocess_list","__python2js_handle_postprocess_list","__python2js_ucs1","__python2js_ucs2","__python2js_ucs4","__python2js_add_to_cache","__python2js_destroy_cache","__python2js_cache_lookup","__JsArray_PushEntry_helper","__JsArray_PostProcess_helper","_python2js__default_converter_js","_python2js_custom__create_jscontext","_destroy_proxies_js","_pyodide_export","_pyodide_js_init","_py_version_major","_py_version_minor","_py_version_micro","___hiwire_deduplicate_new","___hiwire_deduplicate_get","___hiwire_deduplicate_set","___hiwire_deduplicate_delete","___em_lib_deps_libffi","_unbox_small_structs","_ffi_call_js","_ffi_closure_alloc_js","_ffi_closure_free_js","_ffi_prep_closure_loc_js","_fflush","___indirect_function_table","___start_em_js","___stop_em_js","___start_em_lib_deps","___stop_em_lib_deps","___start_em_asm","___stop_em_asm","onRuntimeInitialized"].forEach((prop) => {
  if (!Object.getOwnPropertyDescriptor(Module['ready'], prop)) {
    Object.defineProperty(Module['ready'], prop, {
      get: () => abort('You are getting ' + prop + ' on the Promise object, instead of the instance. Use .then() to get called back with the instance, see the MODULARIZE docs in src/settings.js'),
      set: () => abort('You are setting ' + prop + ' on the Promise object, instead of the instance. Use .then() to get called back with the instance, see the MODULARIZE docs in src/settings.js'),
    });
  }
});

// --pre-jses are emitted after the Module integration code, so that they can
// refer to Module (if they choose; they can also define Module)


// Sometimes an existing Module object exists with properties
// meant to overwrite the default module functionality. Here
// we collect those properties and reapply _after_ we configure
// the current environment's defaults to avoid having to be so
// defensive during initialization.
var moduleOverrides = Object.assign({}, Module);

var arguments_ = [];
var thisProgram = './this.program';
var quit_ = (status, toThrow) => {
  throw toThrow;
};

// Determine the runtime environment we are in. You can customize this by
// setting the ENVIRONMENT setting at compile time (see settings.js).

// Attempt to auto-detect the environment
var ENVIRONMENT_IS_WEB = typeof window == 'object';
var ENVIRONMENT_IS_WORKER = typeof importScripts == 'function';
// N.b. Electron.js environment is simultaneously a NODE-environment, but
// also a web environment.
var ENVIRONMENT_IS_NODE = typeof process == 'object' && typeof process.versions == 'object' && typeof process.versions.node == 'string';
var ENVIRONMENT_IS_SHELL = !ENVIRONMENT_IS_WEB && !ENVIRONMENT_IS_NODE && !ENVIRONMENT_IS_WORKER;

if (Module['ENVIRONMENT']) {
  throw new Error('Module.ENVIRONMENT has been deprecated. To force the environment, use the ENVIRONMENT compile-time option (for example, -sENVIRONMENT=web or -sENVIRONMENT=node)');
}

// `/` should be present at the end if `scriptDirectory` is not empty
var scriptDirectory = '';
function locateFile(path) {
  if (Module['locateFile']) {
    return Module['locateFile'](path, scriptDirectory);
  }
  return scriptDirectory + path;
}

// Hooks that are implemented differently in different runtime environments.
var read_,
    readAsync,
    readBinary,
    setWindowTitle;

if (ENVIRONMENT_IS_NODE) {
  if (typeof process == 'undefined' || !process.release || process.release.name !== 'node') throw new Error('not compiled for this environment (did you build to HTML and try to run it not on the web, or set ENVIRONMENT to something - like node - and run it someplace else - like on the web?)');

  var nodeVersion = process.versions.node;
  var numericVersion = nodeVersion.split('.').slice(0, 3);
  numericVersion = (numericVersion[0] * 10000) + (numericVersion[1] * 100) + (numericVersion[2].split('-')[0] * 1);
  var minVersion = 160000;
  if (numericVersion < 160000) {
    throw new Error('This emscripten-generated code requires node v16.0.0 (detected v' + nodeVersion + ')');
  }

  // `require()` is no-op in an ESM module, use `createRequire()` to construct
  // the require()` function.  This is only necessary for multi-environment
  // builds, `-sENVIRONMENT=node` emits a static import declaration instead.
  // TODO: Swap all `require()`'s with `import()`'s?
  const { createRequire } = await import('module');
  /** @suppress{duplicate} */
  var require = createRequire(import.meta.url);
  // These modules will usually be used on Node.js. Load them eagerly to avoid
  // the complexity of lazy-loading.
  var fs = require('fs');
  var nodePath = require('path');

  if (ENVIRONMENT_IS_WORKER) {
    scriptDirectory = nodePath.dirname(scriptDirectory) + '/';
  } else {
    // EXPORT_ES6 + ENVIRONMENT_IS_NODE always requires use of import.meta.url,
    // since there's no way getting the current absolute path of the module when
    // support for that is not available.
    scriptDirectory = require('url').fileURLToPath(new URL('./', import.meta.url)); // includes trailing slash
  }

// include: node_shell_read.js
read_ = (filename, binary) => {
  // We need to re-wrap `file://` strings to URLs. Normalizing isn't
  // necessary in that case, the path should already be absolute.
  filename = isFileURI(filename) ? new URL(filename) : nodePath.normalize(filename);
  return fs.readFileSync(filename, binary ? undefined : 'utf8');
};

readBinary = (filename) => {
  var ret = read_(filename, true);
  if (!ret.buffer) {
    ret = new Uint8Array(ret);
  }
  assert(ret.buffer);
  return ret;
};

readAsync = (filename, onload, onerror, binary = true) => {
  // See the comment in the `read_` function.
  filename = isFileURI(filename) ? new URL(filename) : nodePath.normalize(filename);
  fs.readFile(filename, binary ? undefined : 'utf8', (err, data) => {
    if (err) onerror(err);
    else onload(binary ? data.buffer : data);
  });
};
// end include: node_shell_read.js
  if (!Module['thisProgram'] && process.argv.length > 1) {
    thisProgram = process.argv[1].replace(/\\/g, '/');
  }

  arguments_ = process.argv.slice(2);

  // MODULARIZE will export the module in the proper place outside, we don't need to export here

  quit_ = (status, toThrow) => {
    process.exitCode = status;
    throw toThrow;
  };

  Module['inspect'] = () => '[Emscripten Module object]';

} else
if (ENVIRONMENT_IS_SHELL) {

  if ((typeof process == 'object' && typeof require === 'function') || typeof window == 'object' || typeof importScripts == 'function') throw new Error('not compiled for this environment (did you build to HTML and try to run it not on the web, or set ENVIRONMENT to something - like node - and run it someplace else - like on the web?)');

  if (typeof read != 'undefined') {
    read_ = read;
  }

  readBinary = (f) => {
    if (typeof readbuffer == 'function') {
      return new Uint8Array(readbuffer(f));
    }
    let data = read(f, 'binary');
    assert(typeof data == 'object');
    return data;
  };

  readAsync = (f, onload, onerror) => {
    setTimeout(() => onload(readBinary(f)));
  };

  if (typeof clearTimeout == 'undefined') {
    globalThis.clearTimeout = (id) => {};
  }

  if (typeof setTimeout == 'undefined') {
    // spidermonkey lacks setTimeout but we use it above in readAsync.
    globalThis.setTimeout = (f) => (typeof f == 'function') ? f() : abort();
  }

  if (typeof scriptArgs != 'undefined') {
    arguments_ = scriptArgs;
  } else if (typeof arguments != 'undefined') {
    arguments_ = arguments;
  }

  if (typeof quit == 'function') {
    quit_ = (status, toThrow) => {
      // Unlike node which has process.exitCode, d8 has no such mechanism. So we
      // have no way to set the exit code and then let the program exit with
      // that code when it naturally stops running (say, when all setTimeouts
      // have completed). For that reason, we must call `quit` - the only way to
      // set the exit code - but quit also halts immediately.  To increase
      // consistency with node (and the web) we schedule the actual quit call
      // using a setTimeout to give the current stack and any exception handlers
      // a chance to run.  This enables features such as addOnPostRun (which
      // expected to be able to run code after main returns).
      setTimeout(() => {
        if (!(toThrow instanceof ExitStatus)) {
          let toLog = toThrow;
          if (toThrow && typeof toThrow == 'object' && toThrow.stack) {
            toLog = [toThrow, toThrow.stack];
          }
          err(`exiting due to exception: ${toLog}`);
        }
        quit(status);
      });
      throw toThrow;
    };
  }

  if (typeof print != 'undefined') {
    // Prefer to use print/printErr where they exist, as they usually work better.
    if (typeof console == 'undefined') console = /** @type{!Console} */({});
    console.log = /** @type{!function(this:Console, ...*): undefined} */ (print);
    console.warn = console.error = /** @type{!function(this:Console, ...*): undefined} */ (typeof printErr != 'undefined' ? printErr : print);
  }

} else

// Note that this includes Node.js workers when relevant (pthreads is enabled).
// Node.js workers are detected as a combination of ENVIRONMENT_IS_WORKER and
// ENVIRONMENT_IS_NODE.
if (ENVIRONMENT_IS_WEB || ENVIRONMENT_IS_WORKER) {
  if (ENVIRONMENT_IS_WORKER) { // Check worker, not web, since window could be polyfilled
    scriptDirectory = self.location.href;
  } else if (typeof document != 'undefined' && document.currentScript) { // web
    scriptDirectory = document.currentScript.src;
  }
  // When MODULARIZE, this JS may be executed later, after document.currentScript
  // is gone, so we saved it, and we use it here instead of any other info.
  if (_scriptDir) {
    scriptDirectory = _scriptDir;
  }
  // blob urls look like blob:http://site.com/etc/etc and we cannot infer anything from them.
  // otherwise, slice off the final part of the url to find the script directory.
  // if scriptDirectory does not contain a slash, lastIndexOf will return -1,
  // and scriptDirectory will correctly be replaced with an empty string.
  // If scriptDirectory contains a query (starting with ?) or a fragment (starting with #),
  // they are removed because they could contain a slash.
  if (scriptDirectory.indexOf('blob:') !== 0) {
    scriptDirectory = scriptDirectory.substr(0, scriptDirectory.replace(/[?#].*/, "").lastIndexOf('/')+1);
  } else {
    scriptDirectory = '';
  }

  if (!(typeof window == 'object' || typeof importScripts == 'function')) throw new Error('not compiled for this environment (did you build to HTML and try to run it not on the web, or set ENVIRONMENT to something - like node - and run it someplace else - like on the web?)');

  // Differentiate the Web Worker from the Node Worker case, as reading must
  // be done differently.
  {
// include: web_or_worker_shell_read.js
read_ = (url) => {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url, false);
    xhr.send(null);
    return xhr.responseText;
  }

  if (ENVIRONMENT_IS_WORKER) {
    readBinary = (url) => {
      var xhr = new XMLHttpRequest();
      xhr.open('GET', url, false);
      xhr.responseType = 'arraybuffer';
      xhr.send(null);
      return new Uint8Array(/** @type{!ArrayBuffer} */(xhr.response));
    };
  }

  readAsync = (url, onload, onerror) => {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url, true);
    xhr.responseType = 'arraybuffer';
    xhr.onload = () => {
      if (xhr.status == 200 || (xhr.status == 0 && xhr.response)) { // file URLs can return 0
        onload(xhr.response);
        return;
      }
      onerror();
    };
    xhr.onerror = onerror;
    xhr.send(null);
  }

// end include: web_or_worker_shell_read.js
  }

  setWindowTitle = (title) => document.title = title;
} else
{
  throw new Error('environment detection error');
}

var out = Module['print'] || console.log.bind(console);
var err = Module['printErr'] || console.error.bind(console);

// Merge back in the overrides
Object.assign(Module, moduleOverrides);
// Free the object hierarchy contained in the overrides, this lets the GC
// reclaim data used e.g. in memoryInitializerRequest, which is a large typed array.
moduleOverrides = null;
checkIncomingModuleAPI();

// Emit code to handle expected values on the Module object. This applies Module.x
// to the proper local x. This has two benefits: first, we only emit it if it is
// expected to arrive, and second, by using a local everywhere else that can be
// minified.

if (Module['arguments']) arguments_ = Module['arguments'];legacyModuleProp('arguments', 'arguments_');

if (Module['thisProgram']) thisProgram = Module['thisProgram'];legacyModuleProp('thisProgram', 'thisProgram');

if (Module['quit']) quit_ = Module['quit'];legacyModuleProp('quit', 'quit_');

// perform assertions in shell.js after we set up out() and err(), as otherwise if an assertion fails it cannot print the message
// Assertions on removed incoming Module JS APIs.
assert(typeof Module['memoryInitializerPrefixURL'] == 'undefined', 'Module.memoryInitializerPrefixURL option was removed, use Module.locateFile instead');
assert(typeof Module['pthreadMainPrefixURL'] == 'undefined', 'Module.pthreadMainPrefixURL option was removed, use Module.locateFile instead');
assert(typeof Module['cdInitializerPrefixURL'] == 'undefined', 'Module.cdInitializerPrefixURL option was removed, use Module.locateFile instead');
assert(typeof Module['filePackagePrefixURL'] == 'undefined', 'Module.filePackagePrefixURL option was removed, use Module.locateFile instead');
assert(typeof Module['read'] == 'undefined', 'Module.read option was removed (modify read_ in JS)');
assert(typeof Module['readAsync'] == 'undefined', 'Module.readAsync option was removed (modify readAsync in JS)');
assert(typeof Module['readBinary'] == 'undefined', 'Module.readBinary option was removed (modify readBinary in JS)');
assert(typeof Module['setWindowTitle'] == 'undefined', 'Module.setWindowTitle option was removed (modify setWindowTitle in JS)');
assert(typeof Module['TOTAL_MEMORY'] == 'undefined', 'Module.TOTAL_MEMORY has been renamed Module.INITIAL_MEMORY');
legacyModuleProp('asm', 'wasmExports');
legacyModuleProp('read', 'read_');
legacyModuleProp('readAsync', 'readAsync');
legacyModuleProp('readBinary', 'readBinary');
legacyModuleProp('setWindowTitle', 'setWindowTitle');
var IDBFS = 'IDBFS is no longer included by default; build with -lidbfs.js';
var PROXYFS = 'PROXYFS is no longer included by default; build with -lproxyfs.js';
var WORKERFS = 'WORKERFS is no longer included by default; build with -lworkerfs.js';
var FETCHFS = 'FETCHFS is no longer included by default; build with -lfetchfs.js';
var ICASEFS = 'ICASEFS is no longer included by default; build with -licasefs.js';
var JSFILEFS = 'JSFILEFS is no longer included by default; build with -ljsfilefs.js';
var OPFS = 'OPFS is no longer included by default; build with -lopfs.js';



assert(!ENVIRONMENT_IS_WORKER, "worker environment detected but not enabled at build time.  Add 'worker' to `-sENVIRONMENT` to enable.");

assert(!ENVIRONMENT_IS_SHELL, "shell environment detected but not enabled at build time.  Add 'shell' to `-sENVIRONMENT` to enable.");


// end include: shell.js
// include: preamble.js
// === Preamble library stuff ===

// Documentation for the public APIs defined in this file must be updated in:
//    site/source/docs/api_reference/preamble.js.rst
// A prebuilt local version of the documentation is available at:
//    site/build/text/docs/api_reference/preamble.js.txt
// You can also build docs locally as HTML or other formats in site/
// An online HTML version (which may be of a different version of Emscripten)
//    is up at http://kripken.github.io/emscripten-site/docs/api_reference/preamble.js.html

var wasmBinary;
if (Module['wasmBinary']) wasmBinary = Module['wasmBinary'];legacyModuleProp('wasmBinary', 'wasmBinary');
var noExitRuntime = Module['noExitRuntime'] || true;legacyModuleProp('noExitRuntime', 'noExitRuntime');

if (typeof WebAssembly != 'object') {
  abort('no native wasm support detected');
}

// Wasm globals

var wasmMemory;

//========================================
// Runtime essentials
//========================================

// whether we are quitting the application. no code should run after this.
// set in exit() and abort()
var ABORT = false;

// set by exit() and abort().  Passed to 'onExit' handler.
// NOTE: This is also used as the process return code code in shell environments
// but only when noExitRuntime is false.
var EXITSTATUS;

/** @type {function(*, string=)} */
function assert(condition, text) {
  if (!condition) {
    abort('Assertion failed' + (text ? ': ' + text : ''));
  }
}

// We used to include malloc/free by default in the past. Show a helpful error in
// builds with assertions.

// Memory management

var HEAP,
/** @type {!Int8Array} */
  HEAP8,
/** @type {!Uint8Array} */
  HEAPU8,
/** @type {!Int16Array} */
  HEAP16,
/** @type {!Uint16Array} */
  HEAPU16,
/** @type {!Int32Array} */
  HEAP32,
/** @type {!Uint32Array} */
  HEAPU32,
/** @type {!Float32Array} */
  HEAPF32,
/* BigInt64Array type is not correctly defined in closure
/** not-@type {!BigInt64Array} */
  HEAP64,
/* BigUInt64Array type is not correctly defined in closure
/** not-t@type {!BigUint64Array} */
  HEAPU64,
/** @type {!Float64Array} */
  HEAPF64;

function updateMemoryViews() {
  var b = wasmMemory.buffer;
  Module['HEAP8'] = HEAP8 = new Int8Array(b);
  Module['HEAP16'] = HEAP16 = new Int16Array(b);
  Module['HEAPU8'] = HEAPU8 = new Uint8Array(b);
  Module['HEAPU16'] = HEAPU16 = new Uint16Array(b);
  Module['HEAP32'] = HEAP32 = new Int32Array(b);
  Module['HEAPU32'] = HEAPU32 = new Uint32Array(b);
  Module['HEAPF32'] = HEAPF32 = new Float32Array(b);
  Module['HEAPF64'] = HEAPF64 = new Float64Array(b);
  Module['HEAP64'] = HEAP64 = new BigInt64Array(b);
  Module['HEAPU64'] = HEAPU64 = new BigUint64Array(b);
}

assert(!Module['STACK_SIZE'], 'STACK_SIZE can no longer be set at runtime.  Use -sSTACK_SIZE at link time')

assert(typeof Int32Array != 'undefined' && typeof Float64Array !== 'undefined' && Int32Array.prototype.subarray != undefined && Int32Array.prototype.set != undefined,
       'JS engine does not provide full typed array support');

// If memory is defined in wasm, the user can't provide it, or set INITIAL_MEMORY
assert(!Module['wasmMemory'], 'Use of `wasmMemory` detected.  Use -sIMPORTED_MEMORY to define wasmMemory externally');
assert(!Module['INITIAL_MEMORY'], 'Detected runtime INITIAL_MEMORY setting.  Use -sIMPORTED_MEMORY to define wasmMemory dynamically');

// include: runtime_init_table.js
// In regular non-RELOCATABLE mode the table is exported
// from the wasm module and this will be assigned once
// the exports are available.
var wasmTable;
// end include: runtime_init_table.js
// include: runtime_stack_check.js
// Initializes the stack cookie. Called at the startup of main and at the startup of each thread in pthreads mode.
function writeStackCookie() {
  var max = _emscripten_stack_get_end();
  assert((max & 3) == 0);
  // If the stack ends at address zero we write our cookies 4 bytes into the
  // stack.  This prevents interference with SAFE_HEAP and ASAN which also
  // monitor writes to address zero.
  if (max == 0) {
    max += 4;
  }
  // The stack grow downwards towards _emscripten_stack_get_end.
  // We write cookies to the final two words in the stack and detect if they are
  // ever overwritten.
  HEAPU32[((max)>>2)] = 0x02135467;
  HEAPU32[(((max)+(4))>>2)] = 0x89BACDFE;
  // Also test the global address 0 for integrity.
  HEAPU32[((0)>>2)] = 1668509029;
}

function checkStackCookie() {
  if (ABORT) return;
  var max = _emscripten_stack_get_end();
  // See writeStackCookie().
  if (max == 0) {
    max += 4;
  }
  var cookie1 = HEAPU32[((max)>>2)];
  var cookie2 = HEAPU32[(((max)+(4))>>2)];
  if (cookie1 != 0x02135467 || cookie2 != 0x89BACDFE) {
    abort(`Stack overflow! Stack cookie has been overwritten at ${ptrToString(max)}, expected hex dwords 0x89BACDFE and 0x2135467, but received ${ptrToString(cookie2)} ${ptrToString(cookie1)}`);
  }
  // Also test the global address 0 for integrity.
  if (HEAPU32[((0)>>2)] != 0x63736d65 /* 'emsc' */) {
    abort('Runtime error: The application has corrupted its heap memory area (address zero)!');
  }
}
// end include: runtime_stack_check.js
// include: runtime_assertions.js
// Endianness check
(function() {
  var h16 = new Int16Array(1);
  var h8 = new Int8Array(h16.buffer);
  h16[0] = 0x6373;
  if (h8[0] !== 0x73 || h8[1] !== 0x63) throw 'Runtime error: expected the system to be little-endian! (Run with -sSUPPORT_BIG_ENDIAN to bypass)';
})();

// end include: runtime_assertions.js
var __ATPRERUN__  = []; // functions called before the runtime is initialized
var __ATINIT__    = []; // functions called during startup
var __ATMAIN__    = []; // functions called when main() is to be run
var __ATEXIT__    = []; // functions called during shutdown
var __ATPOSTRUN__ = []; // functions called after the main() is called

var runtimeInitialized = false;

var runtimeKeepaliveCounter = 0;

function keepRuntimeAlive() {
  return noExitRuntime || runtimeKeepaliveCounter > 0;
}

function preRun() {
  if (Module['preRun']) {
    if (typeof Module['preRun'] == 'function') Module['preRun'] = [Module['preRun']];
    while (Module['preRun'].length) {
      addOnPreRun(Module['preRun'].shift());
    }
  }
  callRuntimeCallbacks(__ATPRERUN__);
}

function initRuntime() {
  assert(!runtimeInitialized);
  runtimeInitialized = true;

  checkStackCookie();

  
if (!Module["noFSInit"] && !FS.init.initialized)
  FS.init();
FS.ignorePermissions = false;

TTY.init();
SOCKFS.root = FS.mount(SOCKFS, {}, null);
PIPEFS.root = FS.mount(PIPEFS, {}, null);
  callRuntimeCallbacks(__ATINIT__);
}

function preMain() {
  checkStackCookie();
  
  callRuntimeCallbacks(__ATMAIN__);
}

function postRun() {
  checkStackCookie();

  if (Module['postRun']) {
    if (typeof Module['postRun'] == 'function') Module['postRun'] = [Module['postRun']];
    while (Module['postRun'].length) {
      addOnPostRun(Module['postRun'].shift());
    }
  }

  callRuntimeCallbacks(__ATPOSTRUN__);
}

function addOnPreRun(cb) {
  __ATPRERUN__.unshift(cb);
}

function addOnInit(cb) {
  __ATINIT__.unshift(cb);
}

function addOnPreMain(cb) {
  __ATMAIN__.unshift(cb);
}

function addOnExit(cb) {
}

function addOnPostRun(cb) {
  __ATPOSTRUN__.unshift(cb);
}

// include: runtime_math.js
// https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Math/imul

// https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Math/fround

// https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Math/clz32

// https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Math/trunc

assert(Math.imul, 'This browser does not support Math.imul(), build with LEGACY_VM_SUPPORT or POLYFILL_OLD_MATH_FUNCTIONS to add in a polyfill');
assert(Math.fround, 'This browser does not support Math.fround(), build with LEGACY_VM_SUPPORT or POLYFILL_OLD_MATH_FUNCTIONS to add in a polyfill');
assert(Math.clz32, 'This browser does not support Math.clz32(), build with LEGACY_VM_SUPPORT or POLYFILL_OLD_MATH_FUNCTIONS to add in a polyfill');
assert(Math.trunc, 'This browser does not support Math.trunc(), build with LEGACY_VM_SUPPORT or POLYFILL_OLD_MATH_FUNCTIONS to add in a polyfill');
// end include: runtime_math.js
// A counter of dependencies for calling run(). If we need to
// do asynchronous work before running, increment this and
// decrement it. Incrementing must happen in a place like
// Module.preRun (used by emcc to add file preloading).
// Note that you can add dependencies in preRun, even though
// it happens right before run - run will be postponed until
// the dependencies are met.
var runDependencies = 0;
var runDependencyWatcher = null;
var dependenciesFulfilled = null; // overridden to take different actions when all run dependencies are fulfilled
var runDependencyTracking = {};

function getUniqueRunDependency(id) {
  var orig = id;
  while (1) {
    if (!runDependencyTracking[id]) return id;
    id = orig + Math.random();
  }
}

function addRunDependency(id) {
  runDependencies++;

  if (Module['monitorRunDependencies']) {
    Module['monitorRunDependencies'](runDependencies);
  }

  if (id) {
    assert(!runDependencyTracking[id]);
    runDependencyTracking[id] = 1;
    if (runDependencyWatcher === null && typeof setInterval != 'undefined') {
      // Check for missing dependencies every few seconds
      runDependencyWatcher = setInterval(() => {
        if (ABORT) {
          clearInterval(runDependencyWatcher);
          runDependencyWatcher = null;
          return;
        }
        var shown = false;
        for (var dep in runDependencyTracking) {
          if (!shown) {
            shown = true;
            err('still waiting on run dependencies:');
          }
          err(`dependency: ${dep}`);
        }
        if (shown) {
          err('(end of list)');
        }
      }, 10000);
    }
  } else {
    err('warning: run dependency added without ID');
  }
}

function removeRunDependency(id) {
  runDependencies--;

  if (Module['monitorRunDependencies']) {
    Module['monitorRunDependencies'](runDependencies);
  }

  if (id) {
    assert(runDependencyTracking[id]);
    delete runDependencyTracking[id];
  } else {
    err('warning: run dependency removed without ID');
  }
  if (runDependencies == 0) {
    if (runDependencyWatcher !== null) {
      clearInterval(runDependencyWatcher);
      runDependencyWatcher = null;
    }
    if (dependenciesFulfilled) {
      var callback = dependenciesFulfilled;
      dependenciesFulfilled = null;
      callback(); // can add another dependenciesFulfilled
    }
  }
}

/** @param {string|number=} what */
function abort(what) {
  if (Module['onAbort']) {
    Module['onAbort'](what);
  }

  what = 'Aborted(' + what + ')';
  // TODO(sbc): Should we remove printing and leave it up to whoever
  // catches the exception?
  err(what);

  ABORT = true;
  EXITSTATUS = 1;

  // Use a wasm runtime error, because a JS error might be seen as a foreign
  // exception, which means we'd run destructors on it. We need the error to
  // simply make the program stop.
  // FIXME This approach does not work in Wasm EH because it currently does not assume
  // all RuntimeErrors are from traps; it decides whether a RuntimeError is from
  // a trap or not based on a hidden field within the object. So at the moment
  // we don't have a way of throwing a wasm trap from JS. TODO Make a JS API that
  // allows this in the wasm spec.

  // Suppress closure compiler warning here. Closure compiler's builtin extern
  // defintion for WebAssembly.RuntimeError claims it takes no arguments even
  // though it can.
  // TODO(https://github.com/google/closure-compiler/pull/3913): Remove if/when upstream closure gets fixed.
  /** @suppress {checkTypes} */
  var e = new WebAssembly.RuntimeError(what);

  readyPromiseReject(e);
  // Throw the error whether or not MODULARIZE is set because abort is used
  // in code paths apart from instantiation where an exception is expected
  // to be thrown when abort is called.
  throw e;
}

// include: memoryprofiler.js
// end include: memoryprofiler.js
// include: URIUtils.js
// Prefix of data URIs emitted by SINGLE_FILE and related options.
var dataURIPrefix = 'data:application/octet-stream;base64,';

// Indicates whether filename is a base64 data URI.
function isDataURI(filename) {
  // Prefix of data URIs emitted by SINGLE_FILE and related options.
  return filename.startsWith(dataURIPrefix);
}

// Indicates whether filename is delivered via file protocol (as opposed to http/https)
function isFileURI(filename) {
  return filename.startsWith('file://');
}
// end include: URIUtils.js
function createExportWrapper(name) {
  return function() {
    assert(runtimeInitialized, `native function \`${name}\` called before runtime initialization`);
    var f = wasmExports[name];
    assert(f, `exported native function \`${name}\` not found`);
    return f.apply(null, arguments);
  };
}

// include: runtime_exceptions.js
// end include: runtime_exceptions.js
var wasmBinaryFile;
if (Module['locateFile']) {
  wasmBinaryFile = 'python.asm.wasm';
  if (!isDataURI(wasmBinaryFile)) {
    wasmBinaryFile = locateFile(wasmBinaryFile);
  }
} else {
  // Use bundler-friendly `new URL(..., import.meta.url)` pattern; works in browsers too.
  wasmBinaryFile = new URL('python.asm.wasm', import.meta.url).href;
}

function getBinarySync(file) {
  if (file == wasmBinaryFile && wasmBinary) {
    return new Uint8Array(wasmBinary);
  }
  if (readBinary) {
    return readBinary(file);
  }
  throw "both async and sync fetching of the wasm failed";
}

function getBinaryPromise(binaryFile) {
  // If we don't have the binary yet, try to load it asynchronously.
  // Fetch has some additional restrictions over XHR, like it can't be used on a file:// url.
  // See https://github.com/github/fetch/pull/92#issuecomment-140665932
  // Cordova or Electron apps are typically loaded from a file:// url.
  // So use fetch if it is available and the url is not a file, otherwise fall back to XHR.
  if (!wasmBinary
      && (ENVIRONMENT_IS_WEB || ENVIRONMENT_IS_WORKER)) {
    if (typeof fetch == 'function'
    ) {
      return fetch(binaryFile, { credentials: 'same-origin' }).then((response) => {
        if (!response['ok']) {
          throw "failed to load wasm binary file at '" + binaryFile + "'";
        }
        return response['arrayBuffer']();
      }).catch(() => getBinarySync(binaryFile));
    }
  }

  // Otherwise, getBinarySync should be able to get it synchronously
  return Promise.resolve().then(() => getBinarySync(binaryFile));
}

function instantiateArrayBuffer(binaryFile, imports, receiver) {
  return getBinaryPromise(binaryFile).then((binary) => {
    return WebAssembly.instantiate(binary, imports);
  }).then((instance) => {
    return instance;
  }).then(receiver, (reason) => {
    err(`failed to asynchronously prepare wasm: ${reason}`);

    // Warn on some common problems.
    if (isFileURI(wasmBinaryFile)) {
      err(`warning: Loading from a file URI (${wasmBinaryFile}) is not supported in most browsers. See https://emscripten.org/docs/getting_started/FAQ.html#how-do-i-run-a-local-webserver-for-testing-why-does-my-program-stall-in-downloading-or-preparing`);
    }
    abort(reason);
  });
}

function instantiateAsync(binary, binaryFile, imports, callback) {
  if (!binary &&
      typeof WebAssembly.instantiateStreaming == 'function' &&
      !isDataURI(binaryFile) &&
      // Avoid instantiateStreaming() on Node.js environment for now, as while
      // Node.js v18.1.0 implements it, it does not have a full fetch()
      // implementation yet.
      //
      // Reference:
      //   https://github.com/emscripten-core/emscripten/pull/16917
      !ENVIRONMENT_IS_NODE &&
      typeof fetch == 'function') {
    return fetch(binaryFile, { credentials: 'same-origin' }).then((response) => {
      // Suppress closure warning here since the upstream definition for
      // instantiateStreaming only allows Promise<Repsponse> rather than
      // an actual Response.
      // TODO(https://github.com/google/closure-compiler/pull/3913): Remove if/when upstream closure is fixed.
      /** @suppress {checkTypes} */
      var result = WebAssembly.instantiateStreaming(response, imports);

      return result.then(
        callback,
        function(reason) {
          // We expect the most common failure cause to be a bad MIME type for the binary,
          // in which case falling back to ArrayBuffer instantiation should work.
          err(`wasm streaming compile failed: ${reason}`);
          err('falling back to ArrayBuffer instantiation');
          return instantiateArrayBuffer(binaryFile, imports, callback);
        });
    });
  }
  return instantiateArrayBuffer(binaryFile, imports, callback);
}

// Create the wasm instance.
// Receives the wasm imports, returns the exports.
function createWasm() {
  // prepare imports
  var info = {
    'env': wasmImports,
    'wasi_snapshot_preview1': wasmImports,
  };
  // Load the wasm module and create an instance of using native support in the JS engine.
  // handle a generated wasm instance, receiving its exports and
  // performing other necessary setup
  /** @param {WebAssembly.Module=} module*/
  function receiveInstance(instance, module) {
    var exports = instance.exports;

    wasmExports = exports;
    

    wasmMemory = wasmExports['memory'];
    Module['wasmMemory'] = wasmMemory;
    assert(wasmMemory, "memory not found in wasm exports");
    // This assertion doesn't hold when emscripten is run in --post-link
    // mode.
    // TODO(sbc): Read INITIAL_MEMORY out of the wasm file in post-link mode.
    //assert(wasmMemory.buffer.byteLength === 25165824);
    updateMemoryViews();

    wasmTable = wasmExports['__indirect_function_table'];
    
    assert(wasmTable, "table not found in wasm exports");

    addOnInit(wasmExports['__wasm_call_ctors']);

    removeRunDependency('wasm-instantiate');
    return exports;
  }
  // wait for the pthread pool (if any)
  addRunDependency('wasm-instantiate');

  // Prefer streaming instantiation if available.
  // Async compilation can be confusing when an error on the page overwrites Module
  // (for example, if the order of elements is wrong, and the one defining Module is
  // later), so we save Module and check it later.
  var trueModule = Module;
  function receiveInstantiationResult(result) {
    // 'result' is a ResultObject object which has both the module and instance.
    // receiveInstance() will swap in the exports (to Module.asm) so they can be called
    assert(Module === trueModule, 'the Module object should not be replaced during async compilation - perhaps the order of HTML elements is wrong?');
    trueModule = null;
    // TODO: Due to Closure regression https://github.com/google/closure-compiler/issues/3193, the above line no longer optimizes out down to the following line.
    // When the regression is fixed, can restore the above PTHREADS-enabled path.
    receiveInstance(result['instance']);
  }

  // User shell pages can write their own Module.instantiateWasm = function(imports, successCallback) callback
  // to manually instantiate the Wasm module themselves. This allows pages to
  // run the instantiation parallel to any other async startup actions they are
  // performing.
  // Also pthreads and wasm workers initialize the wasm instance through this
  // path.
  if (Module['instantiateWasm']) {

    try {
      return Module['instantiateWasm'](info, receiveInstance);
    } catch(e) {
      err(`Module.instantiateWasm callback failed with error: ${e}`);
        // If instantiation fails, reject the module ready promise.
        readyPromiseReject(e);
    }
  }

  // If instantiation fails, reject the module ready promise.
  instantiateAsync(wasmBinary, wasmBinaryFile, info, receiveInstantiationResult).catch(readyPromiseReject);
  return {}; // no exports yet; we'll fill them in later
}

// include: runtime_debug.js
function legacyModuleProp(prop, newName, incomming=true) {
  if (!Object.getOwnPropertyDescriptor(Module, prop)) {
    Object.defineProperty(Module, prop, {
      configurable: true,
      get() {
        let extra = incomming ? ' (the initial value can be provided on Module, but after startup the value is only looked for on a local variable of that name)' : '';
        abort(`\`Module.${prop}\` has been replaced by \`${newName}\`` + extra);

      }
    });
  }
}

function ignoredModuleProp(prop) {
  if (Object.getOwnPropertyDescriptor(Module, prop)) {
    abort(`\`Module.${prop}\` was supplied but \`${prop}\` not included in INCOMING_MODULE_JS_API`);
  }
}

// forcing the filesystem exports a few things by default
function isExportedByForceFilesystem(name) {
  return name === 'FS_createPath' ||
         name === 'FS_createDataFile' ||
         name === 'FS_createPreloadedFile' ||
         name === 'FS_unlink' ||
         name === 'addRunDependency' ||
         // The old FS has some functionality that WasmFS lacks.
         name === 'FS_createLazyFile' ||
         name === 'FS_createDevice' ||
         name === 'removeRunDependency';
}

function missingGlobal(sym, msg) {
  if (typeof globalThis !== 'undefined') {
    Object.defineProperty(globalThis, sym, {
      configurable: true,
      get() {
        warnOnce('`' + sym + '` is not longer defined by emscripten. ' + msg);
        return undefined;
      }
    });
  }
}

missingGlobal('buffer', 'Please use HEAP8.buffer or wasmMemory.buffer');
missingGlobal('asm', 'Please use wasmExports instead');

function missingLibrarySymbol(sym) {
  if (typeof globalThis !== 'undefined' && !Object.getOwnPropertyDescriptor(globalThis, sym)) {
    Object.defineProperty(globalThis, sym, {
      configurable: true,
      get() {
        // Can't `abort()` here because it would break code that does runtime
        // checks.  e.g. `if (typeof SDL === 'undefined')`.
        var msg = '`' + sym + '` is a library symbol and not included by default; add it to your library.js __deps or to DEFAULT_LIBRARY_FUNCS_TO_INCLUDE on the command line';
        // DEFAULT_LIBRARY_FUNCS_TO_INCLUDE requires the name as it appears in
        // library.js, which means $name for a JS name with no prefix, or name
        // for a JS name like _name.
        var librarySymbol = sym;
        if (!librarySymbol.startsWith('_')) {
          librarySymbol = '$' + sym;
        }
        msg += " (e.g. -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE='" + librarySymbol + "')";
        if (isExportedByForceFilesystem(sym)) {
          msg += '. Alternatively, forcing filesystem support (-sFORCE_FILESYSTEM) can export this for you';
        }
        warnOnce(msg);
        return undefined;
      }
    });
  }
  // Any symbol that is not included from the JS libary is also (by definition)
  // not exported on the Module object.
  unexportedRuntimeSymbol(sym);
}

function unexportedRuntimeSymbol(sym) {
  if (!Object.getOwnPropertyDescriptor(Module, sym)) {
    Object.defineProperty(Module, sym, {
      configurable: true,
      get() {
        var msg = "'" + sym + "' was not exported. add it to EXPORTED_RUNTIME_METHODS (see the Emscripten FAQ)";
        if (isExportedByForceFilesystem(sym)) {
          msg += '. Alternatively, forcing filesystem support (-sFORCE_FILESYSTEM) can export this for you';
        }
        abort(msg);
      }
    });
  }
}

// Used by XXXXX_DEBUG settings to output debug messages.
function dbg(text) {
  // TODO(sbc): Make this configurable somehow.  Its not always convenient for
  // logging to show up as warnings.
  console.warn.apply(console, arguments);
}
// end include: runtime_debug.js
// === Body ===

var ASM_CONSTS = {
  8770913: () => { throw new Error("intentionally triggered fatal error!"); },  
 8770970: ($0) => { Hiwire.get_value($0)() },  
 8770993: () => { wasmImports["open64"] = wasmImports["open"]; },  
 8771042: ($0) => { API._pyodide = Hiwire.pop_value($0); }
};
function descr_set_trampoline_call(set,obj,value,closure) { return wasmTable.get(set)(obj, value, closure); }
function descr_get_trampoline_call(get,obj,closure) { return wasmTable.get(get)(obj, closure); }
function _PyCFunctionWithKeywords_TrampolineCall(func,self,args,kw) { return wasmTable.get(func)(self, args, kw); }
function _PyImport_InitFunc_TrampolineCall(func) { return wasmTable.get(func)(); }
function _Py_emscripten_runtime() { var info; if (typeof navigator == 'object') { info = navigator.userAgent; } else if (typeof process == 'object') { info = "Node.js ".concat(process.version); } else { info = "UNKNOWN"; } var len = lengthBytesUTF8(info) + 1; var res = _malloc(len); if (res) stringToUTF8(info, res, len); return res; }
function _Py_CheckEmscriptenSignals_Helper() { if (!Module.Py_EmscriptenSignalBuffer) { return 0; } try { let result = Module.Py_EmscriptenSignalBuffer[0]; Module.Py_EmscriptenSignalBuffer[0] = 0; return result; } catch(e) { return 0; } }
function console_error(msg) { let jsmsg = UTF8ToString(msg); console.error(jsmsg); }
function console_error_obj(obj) { console.error(Hiwire.get_value(obj)); }
function new_error(type,msg,err) { try { return Hiwire.new_value( new API.PythonError(UTF8ToString(type), UTF8ToString(msg), err)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function fail_test() { API.fail_test = true; }
function hiwire_init_js() { try { HEAP32[_Js_undefined / 4] = _hiwire_intern(undefined); HEAP32[_Js_null / 4] = _hiwire_intern(null); HEAP32[_Js_true / 4] = _hiwire_intern((!!1)); HEAP32[_Js_false / 4] = _hiwire_intern((!!0));; Hiwire.new_value = _hiwire_new; Hiwire.new_stack = _hiwire_new; Hiwire.intern_object = _hiwire_intern; Hiwire.num_keys = _hiwire_num_refs; Hiwire.stack_length = () => 0; Hiwire.get_value = _hiwire_get; Hiwire.incref = (x) => { _hiwire_incref(x); return x; }; Hiwire.decref = _hiwire_decref; Hiwire.pop_value = _hiwire_pop; Hiwire.isPromise = function(obj) { try { return !!obj && typeof obj.then === "function"; } catch (e) { return (!!0); } }; API.typedArrayAsUint8Array = function(arg) { if (ArrayBuffer.isView(arg)) { return new Uint8Array(arg.buffer, arg.byteOffset, arg.byteLength); } else { return new Uint8Array(arg); } }; { let dtypes_str = [ "b", "B", "h", "H", "i", "I", "f", "d" ].join(String.fromCharCode(0), ); let dtypes_ptr = stringToNewUTF8(dtypes_str); let dtypes_map = {}; for (let[idx, val] of Object.entries(dtypes_str)) { dtypes_map[val] = dtypes_ptr + Number(idx); } let buffer_datatype_map = new Map([ [ "Int8Array", [ dtypes_map["b"], 1, (!!1) ] ], [ "Uint8Array", [ dtypes_map["B"], 1, (!!1) ] ], [ "Uint8ClampedArray", [ dtypes_map["B"], 1, (!!1) ] ], [ "Int16Array", [ dtypes_map["h"], 2, (!!1) ] ], [ "Uint16Array", [ dtypes_map["H"], 2, (!!1) ] ], [ "Int32Array", [ dtypes_map["i"], 4, (!!1) ] ], [ "Uint32Array", [ dtypes_map["I"], 4, (!!1) ] ], [ "Float32Array", [ dtypes_map["f"], 4, (!!1) ] ], [ "Float64Array", [ dtypes_map["d"], 8, (!!1) ] ], [ "DataView", [ dtypes_map["B"], 1, (!!0) ] ], [ "ArrayBuffer", [ dtypes_map["B"], 1, (!!0) ] ], ]); Module.get_buffer_datatype = function(jsobj) { return buffer_datatype_map.get(jsobj.constructor.name) || [ 0, 0, (!!0) ]; }; } Module.iterObject = function * (object) { for (let k in object) { if (Object.prototype.hasOwnProperty.call(object, k)) { yield k; } } }; return 0; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function hiwire_invalid_ref(type,ref) { API.fail_test = (!!1); if (type === 1 && !ref) { if (_PyErr_Occurred()) { let exc = _wrap_exception(); let e = Hiwire.pop_value(exc); console.error( "Pyodide internal error: Argument to hiwire_get is falsy. This was " + "probably because the Python error indicator was set when get_value was " + "called. The Python error that caused this was:", e ); throw e; } else { const msg = ( "Pyodide internal error: Argument to hiwire_get is falsy (but error " + "indicator is not set)." ); console.error(msg); throw new Error(msg); } } const typestr = { [1]: "get", [2]: "incref", [3]: "decref", }[type]; const msg = ( `hiwire_${typestr} on invalid reference ${ref}. This is most likely due ` + "to use after free. It may also be due to memory corruption." ); console.error(msg); throw new Error(msg); }
function hiwire_to_bool(val) { return !!Hiwire.get_value(val); }
function hiwire_int(val) { try { return Hiwire.new_stack(val); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_int_from_digits(digits,ndigits) { try { let result = BigInt(0); for (let i = 0; i < ndigits; i++) { result += BigInt(HEAPU32[(digits >> 2) + i]) << BigInt(32 * i); } result += BigInt(HEAPU32[(digits >> 2) + ndigits - 1] & 0x80000000) << BigInt(1 + 32 * (ndigits - 1)); if (-Number.MAX_SAFE_INTEGER < result && result < Number.MAX_SAFE_INTEGER) { result = Number(result); } return Hiwire.new_stack(result); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_double(val) { try { return Hiwire.new_stack(val); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_string_utf8(ptr) { try { return Hiwire.new_stack(UTF8ToString(ptr)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_throw_error(iderr) { throw Hiwire.pop_value(iderr); }
function hiwire_call(idfunc,idargs) { try { let jsfunc = Hiwire.get_value(idfunc); let jsargs = Hiwire.get_value(idargs); return Hiwire.new_value(jsfunc(... jsargs)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_call_OneArg(idfunc,idarg) { try { let jsfunc = Hiwire.get_value(idfunc); let jsarg = Hiwire.get_value(idarg); return Hiwire.new_value(jsfunc(jsarg)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_call_bound(idfunc,idthis,idargs) { try { let func = Hiwire.get_value(idfunc); let this_; if (idthis === 0) { this_ = null; } else { this_ = Hiwire.get_value(idthis); } let args = Hiwire.get_value(idargs); return Hiwire.new_value(func.apply(this_, args)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_HasMethod(obj_id,name) { try { let obj = Hiwire.get_value(obj_id); return obj && typeof obj[Hiwire.get_value(name)] === "function"; } catch (e) { ; return (!!0); } }
function hiwire_CallMethodString(idobj,name,idargs) { try { let jsobj = Hiwire.get_value(idobj); let jsname = UTF8ToString(name); let jsargs = Hiwire.get_value(idargs); return Hiwire.new_value(jsobj[jsname](...jsargs)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_CallMethod(idobj,name,idargs) { try { let jsobj = Hiwire.get_value(idobj); let jsname = Hiwire.get_value(name); let jsargs = Hiwire.get_value(idargs); return Hiwire.new_value(jsobj[jsname](... jsargs)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_CallMethod_NoArgs(idobj,name) { try { let jsobj = Hiwire.get_value(idobj); let jsname = Hiwire.get_value(name); return Hiwire.new_value(jsobj[jsname]()); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_CallMethod_OneArg(idobj,name,idarg) { try { let jsobj = Hiwire.get_value(idobj); let jsname = Hiwire.get_value(name); let jsarg = Hiwire.get_value(idarg); return Hiwire.new_value(jsobj[jsname](jsarg)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_construct(idobj,idargs) { try { let jsobj = Hiwire.get_value(idobj); let jsargs = Hiwire.get_value(idargs); return Hiwire.new_value(Reflect.construct(jsobj, jsargs)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_has_length(idobj) { try { let val = Hiwire.get_value(idobj); return (typeof val.size === "number") || (typeof val.length === "number" && typeof val !== "function"); } catch (e) { ; return (!!0); } }
function hiwire_get_length_helper(idobj) { try { let val = Hiwire.get_value(idobj); let result; if (typeof val.size === "number") { result = val.size; } else if (typeof val.length === "number") { result = val.length; } else { return -2; } if(result < 0){ return -3; } if(result > 2147483647){ return -4; } return result; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function hiwire_get_length_string(idobj) { try { const val = Hiwire.get_value(idobj); let result; if (typeof val.size === "number") { result = val.size; } else if (typeof val.length === "number") { result = val.length; } return stringToNewUTF8(" " + result.toString()) } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_get_bool(idobj) { try { let val = Hiwire.get_value(idobj); if (!val) { return (!!0); } if (val.size === 0) { if(/HTML[A-Za-z]*Element/.test(getTypeTag(val))){ return (!!1); } return (!!0); } if (val.length === 0 && JsArray_Check(idobj)) { return (!!0); } if (val.byteLength === 0) { return (!!0); } return (!!1); } catch (e) { ; return (!!0); } }
function hiwire_is_function(idobj) { try { return typeof Hiwire.get_value(idobj) === 'function'; } catch (e) { ; return (!!0); } }
function hiwire_is_generator(idobj) { try { return getTypeTag(Hiwire.get_value(idobj)) === "[object Generator]"; } catch (e) { ; return (!!0); } }
function hiwire_is_async_generator(idobj) { try { return getTypeTag(Hiwire.get_value(idobj)) === "[object AsyncGenerator]"; } catch (e) { ; return (!!0); } }
function hiwire_is_comlink_proxy(idobj) { try { let value = Hiwire.get_value(idobj); return !!(API.Comlink && value[API.Comlink.createEndpoint]); } catch (e) { ; return (!!0); } }
function hiwire_is_error(idobj) { try { let value = Hiwire.get_value(idobj); return !!(value && typeof value.stack === "string" && typeof value.message === "string"); } catch (e) { ; return (!!0); } }
function hiwire_is_promise(idobj) { try { let obj = Hiwire.get_value(idobj); return Hiwire.isPromise(obj); } catch (e) { ; return (!!0); } }
function hiwire_resolve_promise(idobj) { try { let obj = Hiwire.get_value(idobj); let result = Promise.resolve(obj); return Hiwire.new_value(result); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_to_string(idobj) { try { return Hiwire.new_value(Hiwire.get_value(idobj).toString()); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_typeof(idobj) { return Hiwire.new_value(typeof Hiwire.get_value(idobj)); }
function hiwire_constructor_name(idobj) { try { return stringToNewUTF8(Hiwire.get_value(idobj).constructor.name); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_less_than(ida,idb) { try { return !!(Hiwire.get_value(ida) < Hiwire.get_value(idb)); } catch (e) { ; return (!!0); } }
function hiwire_less_than_equal(ida,idb) { try { return !!(Hiwire.get_value(ida) <= Hiwire.get_value(idb)); } catch (e) { ; return (!!0); } }
function hiwire_equal(ida,idb) { try { return !!(Hiwire.get_value(ida) === Hiwire.get_value(idb)); } catch (e) { ; return (!!0); } }
function hiwire_not_equal(ida,idb) { try { return !!(Hiwire.get_value(ida) !== Hiwire.get_value(idb)); } catch (e) { ; return (!!0); } }
function hiwire_greater_than(ida,idb) { try { return !!(Hiwire.get_value(ida) > Hiwire.get_value(idb)); } catch (e) { ; return (!!0); } }
function hiwire_greater_than_equal(ida,idb) { try { return !!(Hiwire.get_value(ida) >= Hiwire.get_value(idb)); } catch (e) { ; return (!!0); } }
function hiwire_reversed_iterator(idarray) { try { if (!Module._reversedIterator) { Module._reversedIterator = class ReversedIterator { constructor(array) { this._array = array; this._i = array.length - 1; } __length_hint__() { return this._array.length; } [Symbol.toStringTag]() { return "ReverseIterator"; } next() { const i = this._i; const a = this._array; const done = i < 0; const value = done ? undefined : a[i]; this._i--; return { done, value }; } }; } let array = Hiwire.get_value(idarray); return Hiwire.new_value(new Module._reversedIterator(array)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function hiwire_assign_to_ptr(idobj,ptr) { try { let jsobj = Hiwire.get_value(idobj); Module.HEAPU8.set(API.typedArrayAsUint8Array(jsobj), ptr); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function hiwire_assign_from_ptr(idobj,ptr) { try { let jsobj = Hiwire.get_value(idobj); API.typedArrayAsUint8Array(jsobj).set( Module.HEAPU8.subarray(ptr, ptr + jsobj.byteLength)); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function hiwire_read_from_file(idobj,fd) { try { let jsobj = Hiwire.get_value(idobj); let uint8_buffer = API.typedArrayAsUint8Array(jsobj); let stream = Module.FS.streams[fd]; Module.FS.read(stream, uint8_buffer, 0, uint8_buffer.byteLength); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function hiwire_write_to_file(idobj,fd) { try { let jsobj = Hiwire.get_value(idobj); let uint8_buffer = API.typedArrayAsUint8Array(jsobj); let stream = Module.FS.streams[fd]; Module.FS.write(stream, uint8_buffer, 0, uint8_buffer.byteLength); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function hiwire_into_file(idobj,fd) { try { let jsobj = Hiwire.get_value(idobj); let uint8_buffer = API.typedArrayAsUint8Array(jsobj); let stream = Module.FS.streams[fd]; Module.FS.write( stream, uint8_buffer, 0, uint8_buffer.byteLength, undefined, (!!1)); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function hiwire_get_buffer_info(idobj,byteLength_ptr,format_ptr,size_ptr,checked_ptr) { let jsobj = Hiwire.get_value(idobj); let byteLength = jsobj.byteLength; let [format_utf8, size, checked] = Module.get_buffer_datatype(jsobj); HEAPU32[(byteLength_ptr >> 2) + 0] = byteLength; HEAPU32[(format_ptr >> 2) + 0] = format_utf8; HEAPU32[(size_ptr >> 2) + 0] = size; HEAPU8[checked_ptr + 0] = checked; }
function hiwire_subarray(idarr,start,end) { try { let jsarr = Hiwire.get_value(idarr); let jssub = jsarr.subarray(start, end); return Hiwire.new_value(jssub); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsArray_Check(idobj) { try { let obj = Hiwire.get_value(idobj); if (Array.isArray(obj)) { return (!!1); } let typeTag = getTypeTag(obj); if(typeTag === "[object HTMLCollection]" || typeTag === "[object NodeList]"){ return (!!1); } if (ArrayBuffer.isView(obj) && obj.constructor.name !== "DataView") { return (!!1); } return (!!0); } catch (e) { ; return (!!0); } }
function JsArray_New() { try { return Hiwire.new_value([]); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsArray_Push(idarr,idval) { try { Hiwire.get_value(idarr).push(Hiwire.get_value(idval)); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsArray_Push_unchecked(idarr,idval) { const arr = Hiwire.get_value(idarr); arr.push(Hiwire.get_value(idval)); return arr.length - 1; }
function JsArray_Extend(idarr,idvals) { try { Hiwire.get_value(idarr).push(... Hiwire.get_value(idvals)); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsArray_Get(idobj,idx) { try { let obj = Hiwire.get_value(idobj); let result = obj[idx]; if (result === undefined && !(idx in obj)) { return (0); } return Hiwire.new_value(result); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsArray_Set(idobj,idx,idval) { try { Hiwire.get_value(idobj)[idx] = Hiwire.get_value(idval); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsArray_Delete(idobj,idx) { try { let obj = Hiwire.get_value(idobj); if (idx < 0 || idx >= obj.length) { return (-1); } obj.splice(idx, 1); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsArray_Splice(idobj,idx) { try { let obj = Hiwire.get_value(idobj); if (idx < 0 || idx >= obj.length) { return 0; } return Hiwire.new_value(obj.splice(idx, 1)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsArray_slice(idobj,length,start,stop,step) { try { let obj = Hiwire.get_value(idobj); let result; if (step === 1) { result = obj.slice(start, stop); } else { result = Array.from({ length }, (_, i) => obj[start + i * step]); } return Hiwire.new_value(result); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsArray_slice_assign(idobj,slicelength,start,stop,step,values_length,values) { try { let obj = Hiwire.get_value(idobj); let jsvalues = []; for(let i = 0; i < values_length; i++){ let ref = _python2js(HEAPU32[(values >> 2) + i]); if(ref === 0){ return -1; } jsvalues.push(Hiwire.pop_value(ref)); } if (step === 1) { obj.splice(start, slicelength, ...jsvalues); } else { if(values !== 0) { for(let i = 0; i < slicelength; i ++){ obj.splice(start + i * step, 1, jsvalues[i]); } } else { for(let i = slicelength - 1; i >= 0; i --){ obj.splice(start + i * step, 1); } } } } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsArray_Clear(idobj) { try { let obj = Hiwire.get_value(idobj); obj.splice(0, obj.length); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsArray_ShallowCopy(idobj) { try { const obj = Hiwire.get_value(idobj); const res = ("slice" in obj) ? obj.slice() : Array.from(obj); return Hiwire.new_value(res); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsObject_New() { try { return Hiwire.new_value({}); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function isReservedWord(word) { if (!Module.pythonReservedWords) { Module.pythonReservedWords = new Set([ "False", "await", "else", "import", "pass", "None", "break", "except", "in", "raise", "True", "class", "finally", "is", "return", "and", "continue", "for", "lambda", "try", "as", "def", "from", "nonlocal", "while", "assert", "del", "global", "not", "with", "async", "elif", "if", "or", "yield", ]) } return Module.pythonReservedWords.has(word); }
function normalizeReservedWords(word) { const noTrailing_ = word.replace(/_*$/, ""); if (!isReservedWord(noTrailing_)) { return word; } if (noTrailing_ !== word) { return word.slice(0, -1); } return word; }
function JsObject_GetString(idobj,ptrkey) { try { const jsobj = Hiwire.get_value(idobj); const jskey = normalizeReservedWords(UTF8ToString(ptrkey)); const result = jsobj[jskey]; if (result === undefined && !(jskey in jsobj)) { return (0); } return Hiwire.new_value(result); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsObject_SetString(idobj,ptrkey,idval) { try { let jsobj = Hiwire.get_value(idobj); let jskey = normalizeReservedWords(UTF8ToString(ptrkey)); let jsval = Hiwire.get_value(idval); jsobj[jskey] = jsval; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsObject_DeleteString(idobj,ptrkey) { try { let jsobj = Hiwire.get_value(idobj); let jskey = normalizeReservedWords(UTF8ToString(ptrkey)); delete jsobj[jskey]; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsObject_Dir(idobj) { try { let jsobj = Hiwire.get_value(idobj); let result = []; do { const names = Object.getOwnPropertyNames(jsobj); result.push(...names.filter( s => { let c = s.charCodeAt(0); return c < 48 || c > 57; } ) .map(word => isReservedWord(word.replace(/_*$/, "")) ? word + "_" : word)); } while (jsobj = Object.getPrototypeOf(jsobj)); return Hiwire.new_value(result); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsObject_Entries(idobj) { try { let jsobj = Hiwire.get_value(idobj); return Hiwire.new_value(Object.entries(jsobj)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsObject_Keys(idobj) { try { let jsobj = Hiwire.get_value(idobj); return Hiwire.new_value(Object.keys(jsobj)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsObject_Values(idobj) { try { let jsobj = Hiwire.get_value(idobj); return Hiwire.new_value(Object.values(jsobj)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsString_InternFromCString(str) { try { let jsstring = UTF8ToString(str); return Hiwire.intern_object(jsstring); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsMap_New() { try { return Hiwire.new_value(new Map()); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsMap_Set(mapid,keyid,valueid) { try { let map = Hiwire.get_value(mapid); let key = Hiwire.get_value(keyid); let value = Hiwire.get_value(valueid); map.set(key, value); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsSet_New() { try { return Hiwire.new_value(new Set()); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsSet_Add(mapid,keyid) { try { let set = Hiwire.get_value(mapid); let key = Hiwire.get_value(keyid); set.add(key); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function js2python_immutable_js(id) { try { let value = Hiwire.get_value(id); let result = Module.js2python_convertImmutable(value, id); if (result !== undefined) { return result; } return 0; } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function js2python_js(id) { try { let value = Hiwire.get_value(id); let result = Module.js2python_convertImmutable(value, id); if (result !== undefined) { return result; } return _JsProxy_create(id); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function js2python_convert(id,depth,default_converter) { try { let defaultConverter = default_converter ? Module.hiwire.get_value(default_converter) : undefined; return Module.js2python_convert(id, { depth, defaultConverter }); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function js2python_init() { try { { 0; let PropagateError = Module._PropagatePythonError; function js2python_string(value) { let max_code_point = 0; let num_code_points = 0; for (let c of value) { num_code_points++; let code_point = c.codePointAt(0); max_code_point = code_point > max_code_point ? code_point : max_code_point; } let result = _PyUnicode_New(num_code_points, max_code_point); if (result === 0) { throw new PropagateError(); } let ptr = _PyUnicode_Data(result); if (max_code_point > 0xffff) { for (let c of value) { HEAPU32[ptr / 4] = c.codePointAt(0); ptr += 4; } } else if (max_code_point > 0xff) { for (let c of value) { HEAPU16[ptr / 2] = c.codePointAt(0); ptr += 2; } } else { for (let c of value) { HEAPU8[ptr] = c.codePointAt(0); ptr += 1; } } return result; } function js2python_bigint(value) { let value_orig = value; let length = 0; if (value < 0) { value = -value; } value <<= BigInt(1); while (value) { length++; value >>= BigInt(32); } let stackTop = stackSave(); let ptr = stackAlloc(length * 4); value = value_orig; for (let i = 0; i < length; i++) { HEAPU32[(ptr >> 2) + i] = Number(value & BigInt(0xffffffff)); value >>= BigInt(32); } let result = __PyLong_FromByteArray( ptr, length * 4 , (!!1) , (!!1) , ); stackRestore(stackTop); return result; } function js2python_convertImmutable(value, id) { let result = js2python_convertImmutableInner(value, id); if (result === 0) { throw new PropagateError(); } return result; } Module.js2python_convertImmutable = js2python_convertImmutable; function js2python_convertImmutableInner(value, id) { let type = typeof value; if (type === "string") { return js2python_string(value); } else if (type === "number") { if (Number.isSafeInteger(value)) { return _PyLong_FromDouble(value); } else { return _PyFloat_FromDouble(value); } } else if (type === "bigint") { return js2python_bigint(value); } else if (value === undefined || value === null) { return __js2python_none(); } else if (value === (!!1)) { return __js2python_true(); } else if (value === (!!0)) { return __js2python_false(); } else if (API.isPyProxy(value)) { const { props, shared } = Module.PyProxy_getAttrs(value); if (props.roundtrip) { if (id === undefined) { id = Hiwire.new_value(value); } return _JsProxy_create(id); } else { return __js2python_pyproxy(shared.ptr); } } return undefined; } function js2python_convertList(obj, context) { let list = _PyList_New(obj.length); if (list === 0) { return 0; } let entryid = 0; let item = 0; try { context.cache.set(obj, list); for (let i = 0; i < obj.length; i++) { entryid = Hiwire.new_value(obj[i]); item = js2python_convert_with_context(entryid, context); _Py_IncRef(item); if (_PyList_SetItem(list, i, item) === -1) { throw new PropagateError(); } Hiwire.decref(entryid); entryid = 0; _Py_DecRef(item); item = 0; } } catch (e) { Hiwire.decref(entryid); _Py_DecRef(item); _Py_DecRef(list); throw e; } return list; } function js2python_convertMap(obj, entries, context) { let dict = _PyDict_New(); if (dict === 0) { return 0; } let key_py = 0; let value_id = 0; let value_py = 0; try { context.cache.set(obj, dict); for (let [key_js, value_js] of entries) { key_py = js2python_convertImmutable(key_js); if (key_py === undefined) { let key_type = (key_js.constructor && key_js.constructor.name) || typeof key_js; throw new Error( `Cannot use key of type ${key_type} as a key to a Python dict`, ); } value_id = Hiwire.new_value(value_js); value_py = js2python_convert_with_context(value_id, context); if (_PyDict_SetItem(dict, key_py, value_py) === -1) { throw new PropagateError(); } _Py_DecRef(key_py); key_py = 0; Hiwire.decref(value_id); value_id = 0; _Py_DecRef(value_py); value_py = 0; } } catch (e) { _Py_DecRef(key_py); Hiwire.decref(value_id); _Py_DecRef(value_py); _Py_DecRef(dict); throw e; } return dict; } function js2python_convertSet(obj, context) { let set = _PySet_New(0); if (set === 0) { return 0; } let key_py = 0; try { context.cache.set(obj, set); for (let key_js of obj) { key_py = js2python_convertImmutable(key_js); if (key_py === undefined) { let key_type = (key_js.constructor && key_js.constructor.name) || typeof key_js; throw new Error( `Cannot use key of type ${key_type} as a key to a Python set`, ); } let errcode = _PySet_Add(set, key_py); if (errcode === -1) { throw new PropagateError(); } _Py_DecRef(key_py); key_py = 0; } } catch (e) { _Py_DecRef(key_py); _Py_DecRef(set); throw e; } return set; } function checkBoolIntCollision(obj, ty) { if (obj.has(1) && obj.has((!!1))) { throw new Error( `Cannot faithfully convert ${ty} into Python since it ` + "contains both 1 and true as keys.", ); } if (obj.has(0) && obj.has((!!0))) { throw new Error( `Cannot faithfully convert ${ty} into Python since it ` + "contains both 0 and false as keys.", ); } } function js2python_convertOther(id, value, context) { let typeTag = getTypeTag(value); if ( Array.isArray(value) || value === "[object HTMLCollection]" || value === "[object NodeList]" ) { return js2python_convertList(value, context); } if (typeTag === "[object Map]" || value instanceof Map) { checkBoolIntCollision(value, "Map"); return js2python_convertMap(value, value.entries(), context); } if (typeTag === "[object Set]" || value instanceof Set) { checkBoolIntCollision(value, "Set"); return js2python_convertSet(value, context); } if ( typeTag === "[object Object]" && (value.constructor === undefined || value.constructor.name === "Object") ) { return js2python_convertMap(value, Object.entries(value), context); } if (typeTag === "[object ArrayBuffer]" || ArrayBuffer.isView(value)) { let [format_utf8, itemsize] = Module.get_buffer_datatype(value); return _JsBuffer_CopyIntoMemoryView( id, value.byteLength, format_utf8, itemsize, ); } return undefined; } function js2python_convert_with_context(id, context) { let value = Hiwire.get_value(id); let result; result = js2python_convertImmutable(value, id); if (result !== undefined) { return result; } if (context.depth === 0) { return _JsProxy_create(id); } result = context.cache.get(value); if (result !== undefined) { return result; } context.depth--; try { result = js2python_convertOther(id, value, context); if (result !== undefined) { return result; } if (context.defaultConverter === undefined) { return _JsProxy_create(id); } let result_js = context.defaultConverter( value, context.converter, context.cacheConversion, ); result = js2python_convertImmutable(result_js); if (API.isPyProxy(result_js)) { Module.pyproxy_destroy(result_js, "", (!!0)); } if (result !== undefined) { return result; } let result_id = Module.hiwire.new_value(result_js); result = _JsProxy_create(result_id); Module.hiwire.decref(result_id); return result; } finally { context.depth++; } } function js2python_convert(id, { depth, defaultConverter }) { let context = { cache: new Map(), depth, defaultConverter, converter(x) { let id = Module.hiwire.new_value(x); try { return Module.pyproxy_new( js2python_convert_with_context(id, context), ); } finally { Module.hiwire.decref(id); } }, cacheConversion(input, output) { if (API.isPyProxy(output)) { context.cache.set(input, Module.PyProxy_getPtr(output)); } else { throw new Error("Second argument should be a PyProxy!"); } }, }; return js2python_convert_with_context(id, context); } Module.js2python_convert = js2python_convert; } return 0; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsProxy_GetIter_js(idobj) { try { let jsobj = Hiwire.get_value(idobj); return Hiwire.new_value(jsobj[Symbol.iterator]()); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function handle_next_result_js(resid,result_ptr,msg) { try { let errmsg; const res = Hiwire.get_value(resid); if(typeof res !== "object") { errmsg = `Result should have type "object" not "${typeof res}"`; } else if(typeof res.done === "undefined") { if (typeof res.then === "function") { errmsg = `Result was a promise, use anext() / asend() / athrow() instead.`; } else { errmsg = `Result has no "done" field.`; } } if (errmsg) { HEAPU32[(msg >> 2) + 0] = stringToNewUTF8(errmsg); return -1; } let result_id = Hiwire.new_value(res.value); HEAPU32[(result_ptr >> 2) + 0] = result_id; return res.done; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsException_new_helper(name_ptr,message_ptr,stack_ptr) { try { let name = UTF8ToString(name_ptr); let message = UTF8ToString(message_ptr); let stack = UTF8ToString(stack_ptr); return Hiwire.new_value(API.deserializeError(name, message, stack)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsProxy_GetAsyncIter_js(idobj) { try { let jsobj = Hiwire.get_value(idobj); return Hiwire.new_value(jsobj[Symbol.asyncIterator]()); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function _agen_handle_result_js(promiseid,msg,set_result,set_exception,closing) { try { let p = Hiwire.get_value(promiseid); let errmsg; if(typeof p !== "object") { errmsg = `Result of anext() should be object not ${typeof p}`; } else if(typeof p.then !== "function") { if (typeof p.done === "boolean") { errmsg = `Result of anext() was not a promise, use next() instead.`; } else { errmsg = `Result of anext() was not a promise.`; } } if (errmsg) { HEAPU32[(msg >> 2) + 0] = stringToNewUTF8(errmsg); return -1; } _Py_IncRef(set_result); _Py_IncRef(set_exception); p.then(({done, value}) => { let id = Hiwire.new_value(value); __agen_handle_result_js_c(set_result, set_exception, done, id, closing); Hiwire.decref(id); }, (err) => { let id = Hiwire.new_value(err); __agen_handle_result_js_c(set_result, set_exception, -1, id, closing); Hiwire.decref(id); }).finally(() => { _Py_DecRef(set_result); _Py_DecRef(set_exception); }); return 0; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function destroy_jsarray_entries(idarray) { for (let v of Hiwire.get_value(idarray)) { try { if(typeof v.destroy === "function"){ v.destroy(); } } catch(e) { console.warn("Weird error:", e); } } }
function JsArray_repeat_js(oid,count) { try { const o = Hiwire.get_value(oid); return Hiwire.new_value(Array.from({ length : count }, () => o).flat()) } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsArray_inplace_repeat_js(oid,count) { try { const o = Hiwire.get_value(oid); o.splice(0, o.length, ... Array.from({ length : count }, () => o).flat()); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsArray_index_helper(list,value,start,stop) { try { let o = Hiwire.get_value(list); let v = Hiwire.get_value(value); for (let i = start; i < stop; i++) { if (o[i] === v) { return i; } } return -1; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsArray_count_helper(list,value) { try { let o = Hiwire.get_value(list); let v = Hiwire.get_value(value); let result = 0; for (let i = 0; i < o.length; i++) { if (o[i] === v) { result++; } } return result; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsArray_reverse_helper(arrayid) { try { Hiwire.get_value(arrayid).reverse(); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsProxy_subscript_js(idobj,idkey) { try { let obj = Hiwire.get_value(idobj); let key = Hiwire.get_value(idkey); let result = obj.get(key); if (result === undefined) { if (obj.has && typeof obj.has === "function" && !obj.has(key)) { return 0; } } return Hiwire.new_value(result); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsMap_GetIter_js(idobj) { try { let jsobj = Hiwire.get_value(idobj); let result; if(typeof jsobj.keys === 'function') { result = jsobj.keys(); } else { result = jsobj[Symbol.iterator](); } return Hiwire.new_value(result); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsMap_clear_js(idmap) { try { const map = Hiwire.get_value(idmap); if(idmap && typeof idmap.clear === "function") { idmap.clear(); return 1; } return 0; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsObjMap_GetIter_js(idobj) { try { let jsobj = Hiwire.get_value(idobj); return Hiwire.new_value(Module.iterObject(jsobj)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsObjMap_length_js(idobj) { try { let jsobj = Hiwire.get_value(idobj); let length = 0; for (let _ of Module.iterObject(jsobj)) { length++; } return length; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsObjMap_subscript_js(idobj,idkey) { try { let obj = Hiwire.get_value(idobj); let key = Hiwire.get_value(idkey); if (!Object.prototype.hasOwnProperty.call(obj, key)) { return 0; } return Hiwire.new_value(obj[key]); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsObjMap_ass_subscript_js(idobj,idkey,idvalue) { try { let obj = Hiwire.get_value(idobj); let key = Hiwire.get_value(idkey); if(idvalue === 0) { if (!Object.prototype.hasOwnProperty.call(obj, key)) { return -1; } delete obj[key]; } else { obj[key] = Hiwire.get_value(idvalue); } return 0; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function JsObjMap_contains_js(idobj,idkey) { try { let obj = Hiwire.get_value(idobj); let key = Hiwire.get_value(idkey); return Object.prototype.hasOwnProperty.call(obj, key); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function get_async_js_call_done_callback(proxies_id) { try { let proxies = Hiwire.get_value(proxies_id); return Hiwire.new_value(function(result) { let msg = "This borrowed proxy was automatically destroyed " + "at the end of an asynchronous function call. Try " + "using create_proxy or create_once_callable."; for (let px of proxies) { Module.pyproxy_destroy(px, msg, (!!0)); } if (API.isPyProxy(result)) { Module.pyproxy_destroy(result, msg, (!!0)); } }); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function wrap_generator(genid,proxiesid) { try { const proxies = new Set(Hiwire.get_value(proxiesid)); const gen = Hiwire.get_value(genid); const msg = "This borrowed proxy was automatically destroyed " + "when a generator completed execution. Try " + "using create_proxy or create_once_callable."; function cleanup() { proxies.forEach((px) => Module.pyproxy_destroy(px, msg)); } function wrap(funcname) { return function (val) { if(API.isPyProxy(val)) { val = val.copy(); proxies.add(val); } let res; try { res = gen[funcname](val); } catch (e) { cleanup(); throw e; } if (res.done) { proxies.delete(res.value); cleanup(); } return res; }; } return Hiwire.new_value({ get [Symbol.toStringTag]() { return "Generator"; }, [Symbol.iterator]() { return this; }, next: wrap("next"), throw: wrap("throw"), return: wrap("return"), }); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function wrap_async_generator(genid,proxiesid) { try { const proxies = new Set(Hiwire.get_value(proxiesid)); const gen = Hiwire.get_value(genid); const msg = "This borrowed proxy was automatically destroyed " + "when an asynchronous generator completed execution. Try " + "using create_proxy or create_once_callable."; function cleanup() { proxies.forEach((px) => Module.pyproxy_destroy(px, msg)); } function wrap(funcname) { return async function (val) { if(API.isPyProxy(val)) { val = val.copy(); proxies.add(val); } let res; try { res = await gen[funcname](val); } catch (e) { cleanup(); throw e; } if (res.done) { proxies.delete(res.value); cleanup(); } return res; }; } return Hiwire.new_value({ get [Symbol.toStringTag]() { return "AsyncGenerator"; }, [Symbol.asyncIterator]() { return this; }, next: wrap("next"), throw: wrap("throw"), return: wrap("return"), }); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsBuffer_DecodeString_js(jsbuffer_id,encoding) { try { let buffer = Hiwire.get_value(jsbuffer_id); let encoding_js; if (encoding) { encoding_js = UTF8ToString(encoding); } let decoder = new TextDecoder(encoding_js, {fatal : (!!1), ignoreBOM: (!!1)}); let res; try { res = decoder.decode(buffer); } catch(e){ if(e instanceof TypeError) { return 0; } throw e; } return Hiwire.new_value(res); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsDoubleProxy_unwrap_helper(id) { try { return Module.PyProxy_getPtr(Hiwire.get_value(id)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function JsProxy_compute_typeflags(idobj) { try { let obj = Hiwire.get_value(idobj); let type_flags = 0; if (API.isPyProxy(obj) && !pyproxyIsAlive(obj)) { return 0; } const typeTag = getTypeTag(obj); function safeBool(cb) { try { return cb(); } catch(e) { return (!!0); } } const isBufferView = safeBool(() => ArrayBuffer.isView(obj)); const isArray = safeBool(() => Array.isArray(obj)); const constructorName = safeBool(() => obj.constructor.name) || ""; if (typeof obj === "function") { type_flags |= (1 << 9); }; if (hasMethod(obj, "then")) { type_flags |= (1 << 7); }; if (hasMethod(obj, Symbol.iterator)) { type_flags |= (1 << 0); }; if (hasMethod(obj, Symbol.asyncIterator)) { type_flags |= (1 << 15); }; if (hasMethod(obj, "next") && (hasMethod(obj, Symbol.iterator) || !hasMethod(obj, Symbol.asyncIterator))) { type_flags |= (1 << 1); }; if (hasMethod(obj, "next") && (!hasMethod(obj, Symbol.iterator) || hasMethod(obj, Symbol.asyncIterator))) { type_flags |= (1 << 18); }; if ((hasProperty(obj, "size")) || (hasProperty(obj, "length") && typeof obj !== "function")) { type_flags |= (1 << 2); }; if (hasMethod(obj, "get")) { type_flags |= (1 << 3); }; if (hasMethod(obj, "set")) { type_flags |= (1 << 4); }; if (hasMethod(obj, "has")) { type_flags |= (1 << 5); }; if (hasMethod(obj, "includes")) { type_flags |= (1 << 6); }; if ((isBufferView || (typeTag === '[object ArrayBuffer]')) && !(type_flags & (1 << 9))) { type_flags |= (1 << 8); }; if (API.isPyProxy(obj)) { type_flags |= (1 << 13); }; if (isArray) { type_flags |= (1 << 10); }; if (typeTag === "[object HTMLCollection]" || typeTag === "[object NodeList]") { type_flags |= (1 << 11); }; if (isBufferView && typeTag !== '[object DataView]') { type_flags |= (1 << 12); }; if (typeTag === "[object Generator]") { type_flags |= (1 << 16); }; if (typeTag === "[object AsyncGenerator]") { type_flags |= (1 << 17); }; if (( hasProperty(obj, "name") && hasProperty(obj, "message") && ( hasProperty(obj, "stack") || constructorName === "DOMException" ) ) && !(type_flags & ((1 << 9) | (1 << 8)))) { type_flags |= (1 << 19); }; return type_flags; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function throw_no_gil() { throw new API.NoGilError("Attempted to use PyProxy when Python GIL not held"); }
function pyproxy_Check(x) { if (x == 0) { return false; } let val = Hiwire.get_value(x); return API.isPyProxy(val); }
function pyproxy_AsPyObject(x) { if (x == 0) { return 0; } let val = Hiwire.get_value(x); if (!API.isPyProxy(val)) { return 0; } return Module.PyProxy_getPtr(val); }
function destroy_proxies(proxies_id,msg_ptr) { let msg = undefined; if (msg_ptr) { msg = Hiwire.get_value(_JsString_FromId(msg_ptr)); } let proxies = Hiwire.get_value(proxies_id); for (let px of proxies) { Module.pyproxy_destroy(px, msg, false); } }
function gc_register_proxies(proxies_id) { let proxies = Hiwire.get_value(proxies_id); for (let px of proxies) { Module.gc_register_proxy(Module.PyProxy_getAttrs(px).shared); } }
function destroy_proxy(proxy_id,msg_ptr) { const px = Module.hiwire.get_value(proxy_id); const { shared, props } = Module.PyProxy_getAttrsQuiet(px); if (!shared.ptr) { return; } if (props.roundtrip) { return; } let msg = undefined; if (msg_ptr) { msg = Hiwire.get_value(_JsString_FromId(msg_ptr)); } Module.pyproxy_destroy(px, msg, false); }
function proxy_cache_get(proxyCacheId,descr) { let proxyCache = Hiwire.get_value(proxyCacheId); let proxyId = proxyCache.get(descr); if (!proxyId) { return undefined; } if (pyproxyIsAlive(Hiwire.get_value(proxyId))) { return proxyId; } else { proxyCache.delete(descr); Hiwire.decref(proxyId); return undefined; } }
function proxy_cache_set(proxyCacheId,descr,proxy) { let proxyCache = Hiwire.get_value(proxyCacheId); proxyCache.set(descr, proxy); }
function array_to_js(array,len) { return Hiwire.new_value( Array.from(HEAP32.subarray(array / 4, array / 4 + len))); }
function pyproxy_new_ex(ptrobj,capture_this,roundtrip,gcRegister) { try { return Hiwire.new_value( Module.pyproxy_new(ptrobj, { props: { captureThis: !!capture_this, roundtrip: !!roundtrip }, gcRegister, }) ); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function pyproxy_new(ptrobj) { try { return Hiwire.new_value(Module.pyproxy_new(ptrobj)); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function create_once_callable(obj) { try { _Py_IncRef(obj); let alreadyCalled = (!!0); function wrapper(... args) { if (alreadyCalled) { throw new Error("OnceProxy can only be called once"); } try { return Module.callPyObject(obj, args); } finally { wrapper.destroy(); } } wrapper.destroy = function() { if (alreadyCalled) { throw new Error("OnceProxy has already been destroyed"); } alreadyCalled = (!!1); Module.finalizationRegistry.unregister(wrapper); _Py_DecRef(obj); }; Module.finalizationRegistry.register(wrapper, [ obj, undefined ], wrapper); return Hiwire.new_value(wrapper); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function create_promise_handles(handle_result,handle_exception,done_callback_id) { try { if (handle_result) { _Py_IncRef(handle_result); } if (handle_exception) { _Py_IncRef(handle_exception); } let done_callback = (x) => {}; if(done_callback_id){ done_callback = Hiwire.get_value(done_callback_id); } let used = (!!0); function checkUsed(){ if (used) { throw new Error("One of the promise handles has already been called."); } } function destroy(){ checkUsed(); used = (!!1); if(handle_result){ _Py_DecRef(handle_result); } if(handle_exception){ _Py_DecRef(handle_exception) } } function onFulfilled(res) { checkUsed(); try { if(handle_result){ return Module.callPyObject(handle_result, [res]); } } finally { done_callback(res); destroy(); } } function onRejected(err) { checkUsed(); try { if(handle_exception){ return Module.callPyObject(handle_exception, [err]); } } finally { done_callback(undefined); destroy(); } } onFulfilled.destroy = destroy; onRejected.destroy = destroy; return Hiwire.new_value( [onFulfilled, onRejected] ); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function _python2js_buffer_inner(buf,itemsize,ndim,format,shape,strides,suboffsets) { try { let converter = Module.get_converter(format, itemsize); let result = Module._python2js_buffer_recursive(buf, 0, { ndim, format, itemsize, shape, strides, suboffsets, converter, }); return Hiwire.new_value(result); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function python2js_buffer_init() { try { { 0; Module.processBufferFormatString = function (formatStr, errorMessage = "") { if (formatStr.length > 2) { throw new Error( "Expected format string to have length <= 2, " + `got '${formatStr}'.` + errorMessage, ); } let formatChar = formatStr.slice(-1); let alignChar = formatStr.slice(0, -1); let bigEndian; switch (alignChar) { case "!": case ">": bigEndian = (!!1); break; case "<": case "@": case "=": case "": bigEndian = (!!0); break; default: throw new Error( `Unrecognized alignment character ${alignChar}.` + errorMessage, ); } let arrayType; switch (formatChar) { case "b": arrayType = Int8Array; break; case "s": case "p": case "c": case "B": case "?": arrayType = Uint8Array; break; case "h": arrayType = Int16Array; break; case "H": arrayType = Uint16Array; break; case "i": case "l": case "n": arrayType = Int32Array; break; case "I": case "L": case "N": case "P": arrayType = Uint32Array; break; case "q": if (globalThis.BigInt64Array === undefined) { throw new Error( "BigInt64Array is not supported on this browser." + errorMessage, ); } arrayType = BigInt64Array; break; case "Q": if (globalThis.BigUint64Array === undefined) { throw new Error( "BigUint64Array is not supported on this browser." + errorMessage, ); } arrayType = BigUint64Array; break; case "f": arrayType = Float32Array; break; case "d": arrayType = Float64Array; break; case "e": throw new Error("Javascript has no Float16 support."); default: throw new Error( `Unrecognized format character '${formatChar}'.` + errorMessage, ); } return [arrayType, bigEndian]; }; Module.python2js_buffer_1d_contiguous = function (ptr, stride, n) { let byteLength = stride * n; return HEAP8.slice(ptr, ptr + byteLength).buffer; }; Module.python2js_buffer_1d_noncontiguous = function ( ptr, stride, suboffset, n, itemsize, ) { let byteLength = itemsize * n; let buffer = new Uint8Array(byteLength); for (let i = 0; i < n; ++i) { let curptr = ptr + i * stride; if (suboffset >= 0) { curptr = HEAPU32[(curptr >> 2) + 0] + suboffset; } buffer.set(HEAP8.subarray(curptr, curptr + itemsize), i * itemsize); } return buffer.buffer; }; Module._python2js_buffer_recursive = function (ptr, curdim, bufferData) { let n = HEAPU32[(bufferData.shape >> 2) + curdim]; let stride = HEAP32[(bufferData.strides >> 2) + curdim]; let suboffset = -1; if (bufferData.suboffsets !== 0) { suboffset = HEAP32[(bufferData.suboffsets >> 2) + curdim]; } if (curdim === bufferData.ndim - 1) { let arraybuffer; if (stride === bufferData.itemsize && suboffset < 0) { arraybuffer = Module.python2js_buffer_1d_contiguous(ptr, stride, n); } else { arraybuffer = Module.python2js_buffer_1d_noncontiguous( ptr, stride, suboffset, n, bufferData.itemsize, ); } return bufferData.converter(arraybuffer); } let result = []; for (let i = 0; i < n; ++i) { let curPtr = ptr + i * stride; if (suboffset >= 0) { curptr = HEAPU32[(curptr >> 2) + 0] + suboffset; } result.push( Module._python2js_buffer_recursive(curPtr, curdim + 1, bufferData), ); } return result; }; Module.get_converter = function (format, itemsize) { let formatStr = UTF8ToString(format); let [ArrayType, bigEndian] = Module.processBufferFormatString(formatStr); let formatChar = formatStr.slice(-1); switch (formatChar) { case "s": let decoder = new TextDecoder("utf8", { ignoreBOM: (!!1) }); return (buff) => decoder.decode(buff); case "?": return (buff) => Array.from(new Uint8Array(buff), (x) => !!x); } if (!bigEndian) { return (buff) => new ArrayType(buff); } let getFuncName; let setFuncName; switch (itemsize) { case 2: getFuncName = "getUint16"; setFuncName = "setUint16"; break; case 4: getFuncName = "getUint32"; setFuncName = "setUint32"; break; case 8: getFuncName = "getFloat64"; setFuncName = "setFloat64"; break; default: throw new Error(`Unexpected size ${itemsize}`); } function swapFunc(buff) { let dataview = new DataView(buff); let getFunc = dataview[getFuncName].bind(dataview); let setFunc = dataview[setFuncName].bind(dataview); for (let byte = 0; byte < dataview.byteLength; byte += itemsize) { setFunc(byte, getFunc(byte, (!!1)), (!!0)); } return buff; } return (buff) => new ArrayType(swapFunc(buff)); }; } return 0; } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function _python2js_addto_postprocess_list(idlist,idparent,idkey,value) { const list = Hiwire.get_value(idlist); const parent = Hiwire.get_value(idparent); const key = Hiwire.get_value(idkey); list.push([ parent, key, value ]); }
function _python2js_handle_postprocess_list(idlist,idcache) { const list = Hiwire.get_value(idlist); const cache = Hiwire.get_value(idcache); for (const[parent, key, value] of list) { let out_value = Hiwire.get_value(cache.get(value)); if(parent.constructor.name === "Map"){ parent.set(key, out_value) } else { parent[key] = out_value; } } }
function _python2js_ucs1(ptr,len) { try { let jsstr = ""; for (let i = 0; i < len; ++i) { jsstr += String.fromCharCode(HEAPU8[ptr + i]); } return Hiwire.new_value(jsstr); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function _python2js_ucs2(ptr,len) { try { let jsstr = ""; for (let i = 0; i < len; ++i) { jsstr += String.fromCharCode(HEAPU16[(ptr >> 1) + i]); } return Hiwire.new_value(jsstr); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function _python2js_ucs4(ptr,len) { try { let jsstr = ""; for (let i = 0; i < len; ++i) { jsstr += String.fromCodePoint(HEAPU32[(ptr >> 2) + i]); } return Hiwire.new_value(jsstr); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function _python2js_add_to_cache(cacheid,pyparent,jsparent) { try { const cache = Hiwire.get_value(cacheid); const old_value = cache.get(pyparent); if (old_value !== undefined) { Hiwire.decref(old_value); } cache.set(pyparent, Hiwire.incref(jsparent)); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function _python2js_destroy_cache(cacheid) { const cache = Hiwire.get_value(cacheid); for (const[k, v] of cache.entries()) { Hiwire.decref(v); } }
function _python2js_cache_lookup(cacheid,pyparent) { return Hiwire.get_value(cacheid).get(pyparent); }
function _JsArray_PushEntry_helper(array,key,value) { try { Hiwire.get_value(array).push( [ Hiwire.get_value(key), Hiwire.get_value(value) ]); } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function _JsArray_PostProcess_helper(jscontext,array) { try { return Hiwire.new_value( Hiwire.get_value(jscontext).dict_converter(Hiwire.get_value(array))); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function python2js__default_converter_js(jscontext,object) { try { let context = Hiwire.get_value(jscontext); let proxy = Module.pyproxy_new(object); let result = context.default_converter( proxy, context.converter, context.cacheConversion ); proxy.destroy(); return Hiwire.new_value(result); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function python2js_custom__create_jscontext(context,idcache,dict_converter,default_converter) { try { let jscontext = {}; if (dict_converter !== 0) { jscontext.dict_converter = Hiwire.get_value(dict_converter); } if (default_converter !== 0) { jscontext.default_converter = Hiwire.get_value(default_converter); jscontext.cacheConversion = function (input, output) { if (!API.isPyProxy(input)) { throw new TypeError("The first argument to cacheConversion must be a PyProxy."); } let input_ptr = Module.PyProxy_getPtr(input); let output_key = Hiwire.new_value(output); Hiwire.get_value(idcache).set(input_ptr, output_key); }; jscontext.converter = function (x) { if (!API.isPyProxy(x)) { return x; } let ptr = Module.PyProxy_getPtr(x); let res = __python2js(context, ptr); return Hiwire.pop_value(res); }; } return Hiwire.new_value(jscontext); } catch (e) { ; Module.handle_js_error(e); return 0; } errNoRet(); }
function destroy_proxies_js(proxies_id) { try { for (let proxy of Hiwire.get_value(proxies_id)) { proxy.destroy(); } } catch (e) { ; Module.handle_js_error(e); return -1; } return 0; }
function pyodide_js_init() {
"use strict";(()=>{var Vr=Object.create;var ue=Object.defineProperty;var sr=Object.getOwnPropertyDescriptor;var Kr=Object.getOwnPropertyNames;var qr=Object.getPrototypeOf,Jr=Object.prototype.hasOwnProperty;var a=(r,e)=>ue(r,"name",{value:e,configurable:!0}),je=(r=>typeof require<"u"?require:typeof Proxy<"u"?new Proxy(r,{get:(e,t)=>(typeof require<"u"?require:e)[t]}):r)(function(r){if(typeof require<"u")return require.apply(this,arguments);throw new Error('Dynamic require of "'+r+'" is not supported')});var lr=(r,e)=>()=>(e||r((e={exports:{}}).exports,e),e.exports);var Yr=(r,e,t,n)=>{if(e&&typeof e=="object"||typeof e=="function")for(let o of Kr(e))!Jr.call(r,o)&&o!==t&&ue(r,o,{get:()=>e[o],enumerable:!(n=sr(e,o))||n.enumerable});return r};var Xr=(r,e,t)=>(t=r!=null?Vr(qr(r)):{},Yr(e||!r||!r.__esModule?ue(t,"default",{value:r,enumerable:!0}):t,r));var S=(r,e,t,n)=>{for(var o=n>1?void 0:n?sr(e,t):e,s=r.length-1,i;s>=0;s--)(i=r[s])&&(o=(n?i(e,t,o):i(o))||o);return n&&o&&ue(e,t,o),o};var wr=lr((Ye,xr)=>{(function(r,e){"use strict";typeof define=="function"&&define.amd?define("stackframe",[],e):typeof Ye=="object"?xr.exports=e():r.StackFrame=e()})(Ye,function(){"use strict";function r(d){return!isNaN(parseFloat(d))&&isFinite(d)}a(r,"_isNumber");function e(d){return d.charAt(0).toUpperCase()+d.substring(1)}a(e,"_capitalize");function t(d){return function(){return this[d]}}a(t,"_getter");var n=["isConstructor","isEval","isNative","isToplevel"],o=["columnNumber","lineNumber"],s=["fileName","functionName","source"],i=["args"],u=["evalOrigin"],l=n.concat(o,s,i,u);function c(d){if(d)for(var m=0;m<l.length;m++)d[l[m]]!==void 0&&this["set"+e(l[m])](d[l[m]])}a(c,"StackFrame"),c.prototype={getArgs:function(){return this.args},setArgs:function(d){if(Object.prototype.toString.call(d)!=="[object Array]")throw new TypeError("Args must be an Array");this.args=d},getEvalOrigin:function(){return this.evalOrigin},setEvalOrigin:function(d){if(d instanceof c)this.evalOrigin=d;else if(d instanceof Object)this.evalOrigin=new c(d);else throw new TypeError("Eval Origin must be an Object or StackFrame")},toString:function(){var d=this.getFileName()||"",m=this.getLineNumber()||"",w=this.getColumnNumber()||"",T=this.getFunctionName()||"";return this.getIsEval()?d?"[eval] ("+d+":"+m+":"+w+")":"[eval]:"+m+":"+w:T?T+" ("+d+":"+m+":"+w+")":d+":"+m+":"+w}},c.fromString=a(function(m){var w=m.indexOf("("),T=m.lastIndexOf(")"),Le=m.substring(0,w),ce=m.substring(w+1,T).split(","),L=m.substring(T+1);if(L.indexOf("@")===0)var A=/@(.+?)(?::(\d+))?(?::(\d+))?$/.exec(L,""),k=A[1],R=A[2],$e=A[3];return new c({functionName:Le,args:ce||void 0,fileName:k,lineNumber:R||void 0,columnNumber:$e||void 0})},"StackFrame$$fromString");for(var y=0;y<n.length;y++)c.prototype["get"+e(n[y])]=t(n[y]),c.prototype["set"+e(n[y])]=function(d){return function(m){this[d]=!!m}}(n[y]);for(var p=0;p<o.length;p++)c.prototype["get"+e(o[p])]=t(o[p]),c.prototype["set"+e(o[p])]=function(d){return function(m){if(!r(m))throw new TypeError(d+" must be a Number");this[d]=Number(m)}}(o[p]);for(var _=0;_<s.length;_++)c.prototype["get"+e(s[_])]=t(s[_]),c.prototype["set"+e(s[_])]=function(d){return function(m){this[d]=String(m)}}(s[_]);return c})});var Ar=lr((Xe,vr)=>{(function(r,e){"use strict";typeof define=="function"&&define.amd?define("error-stack-parser",["stackframe"],e):typeof Xe=="object"?vr.exports=e(wr()):r.ErrorStackParser=e(r.StackFrame)})(Xe,a(function(e){"use strict";var t=/(^|@)\S+:\d+/,n=/^\s*at .*(\S+:\d+|\(native\))/m,o=/^(eval@)?(\[native code])?$/;return{parse:a(function(i){if(typeof i.stacktrace<"u"||typeof i["opera#sourceloc"]<"u")return this.parseOpera(i);if(i.stack&&i.stack.match(n))return this.parseV8OrIE(i);if(i.stack)return this.parseFFOrSafari(i);throw new Error("Cannot parse given Error object")},"ErrorStackParser$$parse"),extractLocation:a(function(i){if(i.indexOf(":")===-1)return[i];var u=/(.+?)(?::(\d+))?(?::(\d+))?$/,l=u.exec(i.replace(/[()]/g,""));return[l[1],l[2]||void 0,l[3]||void 0]},"ErrorStackParser$$extractLocation"),parseV8OrIE:a(function(i){var u=i.stack.split(`
`).filter(function(l){return!!l.match(n)},this);return u.map(function(l){l.indexOf("(eval ")>-1&&(l=l.replace(/eval code/g,"eval").replace(/(\(eval at [^()]*)|(,.*$)/g,""));var c=l.replace(/^\s+/,"").replace(/\(eval code/g,"(").replace(/^.*?\s+/,""),y=c.match(/ (\(.+\)$)/);c=y?c.replace(y[0],""):c;var p=this.extractLocation(y?y[1]:c),_=y&&c||void 0,d=["eval","<anonymous>"].indexOf(p[0])>-1?void 0:p[0];return new e({functionName:_,fileName:d,lineNumber:p[1],columnNumber:p[2],source:l})},this)},"ErrorStackParser$$parseV8OrIE"),parseFFOrSafari:a(function(i){var u=i.stack.split(`
`).filter(function(l){return!l.match(o)},this);return u.map(function(l){if(l.indexOf(" > eval")>-1&&(l=l.replace(/ line (\d+)(?: > eval line \d+)* > eval:\d+:\d+/g,":$1")),l.indexOf("@")===-1&&l.indexOf(":")===-1)return new e({functionName:l});var c=/((.*".+"[^@]*)?[^@]*)(?:@)/,y=l.match(c),p=y&&y[1]?y[1]:void 0,_=this.extractLocation(l.replace(c,""));return new e({functionName:p,fileName:_[0],lineNumber:_[1],columnNumber:_[2],source:l})},this)},"ErrorStackParser$$parseFFOrSafari"),parseOpera:a(function(i){return!i.stacktrace||i.message.indexOf(`
`)>-1&&i.message.split(`
`).length>i.stacktrace.split(`
`).length?this.parseOpera9(i):i.stack?this.parseOpera11(i):this.parseOpera10(i)},"ErrorStackParser$$parseOpera"),parseOpera9:a(function(i){for(var u=/Line (\d+).*script (?:in )?(\S+)/i,l=i.message.split(`
`),c=[],y=2,p=l.length;y<p;y+=2){var _=u.exec(l[y]);_&&c.push(new e({fileName:_[2],lineNumber:_[1],source:l[y]}))}return c},"ErrorStackParser$$parseOpera9"),parseOpera10:a(function(i){for(var u=/Line (\d+).*script (?:in )?(\S+)(?:: In function (\S+))?$/i,l=i.stacktrace.split(`
`),c=[],y=0,p=l.length;y<p;y+=2){var _=u.exec(l[y]);_&&c.push(new e({functionName:_[3]||void 0,fileName:_[2],lineNumber:_[1],source:l[y]}))}return c},"ErrorStackParser$$parseOpera10"),parseOpera11:a(function(i){var u=i.stack.split(`
`).filter(function(l){return!!l.match(t)&&!l.match(/^Error created at/)},this);return u.map(function(l){var c=l.split("@"),y=this.extractLocation(c.pop()),p=c.shift()||"",_=p.replace(/<anonymous function(: (\w+))?>/,"$2").replace(/\([^)]*\)/g,"")||void 0,d;p.match(/\(([^)]*)\)/)&&(d=p.replace(/^[^(]+\(([^)]*)\)$/,"$1"));var m=d===void 0||d==="[arguments not available]"?void 0:d.split(",");return new e({functionName:_,args:m,fileName:y[0],lineNumber:y[1],columnNumber:y[2],source:l})},this)},"ErrorStackParser$$parseOpera11")}},"ErrorStackParser"))});var P=typeof process=="object"&&typeof process.versions=="object"&&typeof process.versions.node=="string"&&typeof process.browser=="undefined",Qr,cr,Zr,et,pe;function rt(r,e){return Zr.resolve(e||".",r)}a(rt,"node_resolvePath");function tt(r,e){return e===void 0&&(e=location),new URL(r,e).toString()}a(tt,"browser_resolvePath");var ye;P?ye=rt:ye=tt;var nt;P||(nt="/");function ot(r,e){return r.startsWith("file://")&&(r=r.slice(7)),r.includes("://")?{response:cr(r)}:{binary:pe.readFile(r).then(t=>new Uint8Array(t.buffer,t.byteOffset,t.byteLength))}}a(ot,"node_getBinaryResponse");function at(r,e){let t=new URL(r,location);return{response:fetch(t,e?{integrity:e}:{})}}a(at,"browser_getBinaryResponse");var de;P?de=ot:de=at;async function D(r,e){let{response:t,binary:n}=de(r,e);if(n)return n;let o=await t;if(!o.ok)throw new Error(`Failed to load '${r}': request failed.`);return new Uint8Array(await o.arrayBuffer())}a(D,"loadBinaryFile");var We;if(globalThis.document)We=a(async r=>await import(r),"loadScript");else if(globalThis.importScripts)We=a(async r=>{try{globalThis.importScripts(r)}catch(e){if(e instanceof TypeError)await import(r);else throw e}},"loadScript");else if(P)We=it;else throw new Error("Cannot determine runtime environment");async function it(r){r.startsWith("file://")&&(r=r.slice(7)),r.includes("://")?et.runInThisContext(await(await cr(r)).text()):await import(Qr.pathToFileURL(r).href)}a(it,"nodeLoadScript");function st(r){return Buffer.from(r,"hex").toString("base64")}a(st,"nodeBase16ToBase64");function lt(r){return btoa(r.match(/\w{2}/g).map(function(e){return String.fromCharCode(parseInt(e,16))}).join(""))}a(lt,"browserBase16ToBase64");var ur=P?st:lt;var yr=a(r=>{let e={};return(...t)=>{let n=t[0];if(n in e)return e[n];{let o=r(n);return e[n]=o,o}}},"memoize");function dr(r){let e=!1;return function(){e||(e=!0,console.warn(r))}}a(dr,"makeWarnOnce");function I(r){let e=!1;return function(t,n,o){let s=o.value?"value":"get",i=o[s];return o[s]=function(...u){return e||(e=!0,console.warn(r)),i.call(this,...u)},o}}a(I,"warnOnce");function ct(r){try{return r instanceof f}catch(e){return!1}}a(ct,"isPyProxy");API.isPyProxy=ct;globalThis.FinalizationRegistry?Module.finalizationRegistry=new FinalizationRegistry(({ptr:r,cache:e})=>{e&&(e.leaked=!0,br(e));try{_check_gil(),_Py_DecRef(r)}catch(t){API.fatal_error(t)}}):Module.finalizationRegistry={register(){},unregister(){}};var ze=new Map;Module.pyproxy_alloc_map=ze;var Ve,Ke;Module.enable_pyproxy_allocation_tracing=function(){Ve=a(function(r){ze.set(r,Error().stack)},"trace_pyproxy_alloc"),Ke=a(function(r){ze.delete(r)},"trace_pyproxy_dealloc")};Module.disable_pyproxy_allocation_tracing=function(){Ve=a(function(r){},"trace_pyproxy_alloc"),Ke=a(function(r){},"trace_pyproxy_dealloc")};Module.disable_pyproxy_allocation_tracing();var _r=Symbol("pyproxy.attrs");function ke(r,{flags:e,cache:t,props:n,shared:o,gcRegister:s}={}){s===void 0&&(s=!0);let i=e!==void 0?e:_pyproxy_getflags(r);i===-1&&_pythonexc2js();let u=i&1<<13,l=Module.getPyProxyClass(i),c;i&1<<8?(c=a(function(){},"target"),Object.setPrototypeOf(c,l.prototype),delete c.length,delete c.name,c.prototype=void 0):c=Object.create(l.prototype);let y=!!o;o||(t||(t={cacheId:Hiwire.new_value(new Map),refcnt:0}),t.refcnt++,o={ptr:r,cache:t,flags:i,promise:void 0,destroyed_msg:void 0,gcRegistered:!1},_Py_IncRef(r)),n=Object.assign({isBound:!1,captureThis:!1,boundArgs:[],roundtrip:!1},n);let p=new Proxy(c,u?Pt:M);!y&&s&&gr(o),y||Ve(p);let _={shared:o,props:n};return c[_r]=_,p}a(ke,"pyproxy_new");Module.pyproxy_new=ke;function gr(r){let e=Object.assign({},r);r.gcRegistered=!0,Module.finalizationRegistry.register(r,e,r)}a(gr,"gc_register_proxy");Module.gc_register_proxy=gr;function Ee(r){return r[_r]}a(Ee,"_getAttrsQuiet");Module.PyProxy_getAttrsQuiet=Ee;function O(r){let e=Ee(r);if(!e.shared.ptr)throw new Error(e.shared.destroyed_msg);return e}a(O,"_getAttrs");Module.PyProxy_getAttrs=O;function b(r){return O(r).shared.ptr}a(b,"_getPtr");function g(r){return Object.getPrototypeOf(r).$$flags}a(g,"_getFlags");function pr(r,e,t){let{captureThis:n,boundArgs:o,boundThis:s,isBound:i}=O(r).props;return n?i?[s].concat(o,t):[e].concat(t):i?o.concat(t):t}a(pr,"_adjustArgs");var fr=new Map;Module.getPyProxyClass=function(r){let e=[[1,me],[2,j],[4,N],[8,_e],[16,ge],[32,he],[2048,Pe],[512,be],[1024,xe],[4096,we],[64,Se],[128,Ie],[256,re],[8192,ve],[16384,Ae]],t=fr.get(r);if(t)return t;let n={};for(let[u,l]of e)r&u&&Object.assign(n,Object.getOwnPropertyDescriptors(l.prototype));n.constructor=Object.getOwnPropertyDescriptor(f.prototype,"constructor"),Object.assign(n,Object.getOwnPropertyDescriptors({$$flags:r}));let o=r&1<<8?Pr:hr,s=Object.create(o,n);function i(){}return a(i,"NewPyProxyClass"),i.prototype=s,fr.set(r,i),i};Module.PyProxy_getPtr=b;var ut="This borrowed attribute proxy was automatically destroyed in the process of destroying the proxy it was borrowed from. Try using the 'copy' method.";function br(r){if(r&&(r.refcnt--,r.refcnt===0)){let e=Hiwire.pop_value(r.cacheId);for(let t of e.values()){let n=Hiwire.pop_value(t);r.leaked||Module.pyproxy_destroy(n,ut,!0)}}}a(br,"pyproxy_decref_cache");function yt(r,e){if(e=e||"Object has already been destroyed",API.debug_ffi){let t=r.type,n;try{n=r.toString()}catch(o){if(o.pyodide_fatal_error)throw o}e+=`
The object was of type "${t}" and `,n?e+=`had repr "${n}"`:e+="an error was raised when trying to generate its repr"}else e+="\nFor more information about the cause of this error, use `pyodide.setDebug(true)`";return e}a(yt,"generateDestroyedMessage");Module.pyproxy_destroy=function(r,e,t){let{shared:n,props:o}=Ee(r);if(!n.ptr||!t&&o.roundtrip)return;n.destroyed_msg=yt(r,e);let s=n.ptr;n.ptr=0,n.gcRegistered&&Module.finalizationRegistry.unregister(n),br(n.cache);try{_check_gil(),_Py_DecRef(s),Ke(r)}catch(i){API.fatal_error(i)}};Module.callPyObjectKwargs=function(r,e,t){let n=e.length,o=Object.keys(t),s=Object.values(t),i=o.length;e.push(...s);let u=Hiwire.new_value(e),l=Hiwire.new_value(o),c;try{_check_gil(),c=__pyproxy_apply(r,u,n,l,i)}catch(p){API.maybe_fatal_error(p);return}finally{Hiwire.decref(u),Hiwire.decref(l)}c===0&&_pythonexc2js();let y=Hiwire.pop_value(c);return y&&y.type==="coroutine"&&y._ensure_future&&(_check_gil(),__iscoroutinefunction(r)&&y._ensure_future()),y};Module.callPyObject=function(r,e){return Module.callPyObjectKwargs(r,e,{})};var Je=class{static[Symbol.hasInstance](e){return[Je,qe].some(t=>Function.prototype[Symbol.hasInstance].call(t,e))}constructor(){throw new TypeError("PyProxy is not a constructor")}get[Symbol.toStringTag](){return"PyProxy"}get type(){let e=b(this);return Hiwire.pop_value(__pyproxy_type(e))}toString(){let e=b(this),t;try{_check_gil(),t=__pyproxy_repr(e)}catch(n){API.fatal_error(n)}return t===0&&_pythonexc2js(),Hiwire.pop_value(t)}destroy(e={}){e=Object.assign({message:"",destroyRoundtrip:!0},e);let{message:t,destroyRoundtrip:n}=e;Module.pyproxy_destroy(this,t,n)}copy(){let e=O(this);return ke(e.shared.ptr,{flags:g(this),cache:e.shared.cache,props:e.props})}toJs({depth:e=-1,pyproxies:t=void 0,create_pyproxies:n=!0,dict_converter:o=void 0,default_converter:s=void 0}={}){let i=b(this),u,l,c=0,y=0;n?t?l=Hiwire.new_value(t):l=Hiwire.new_value([]):l=0,o&&(c=Hiwire.new_value(o)),s&&(y=Hiwire.new_value(s));try{_check_gil(),u=_python2js_custom(i,e,l,c,y)}catch(p){API.fatal_error(p)}finally{Hiwire.decref(l),Hiwire.decref(c),Hiwire.decref(y)}return u===0&&_pythonexc2js(),Hiwire.pop_value(u)}supportsLength(){return!!(g(this)&1<<0)}supportsGet(){return!!(g(this)&1<<1)}supportsSet(){return!!(g(this)&1<<2)}supportsHas(){return!!(g(this)&1<<3)}isIterable(){return!!(g(this)&(1<<4|1<<5))}isIterator(){return!!(g(this)&1<<5)}isAwaitable(){return!!(g(this)&1<<6)}isBuffer(){return!!(g(this)&1<<7)}isCallable(){return!!(g(this)&1<<8)}},f=Je;a(f,"PyProxy"),S([I("supportsLength() is deprecated. Use `instanceof pyodide.ffi.PyProxyWithLength` instead.")],f.prototype,"supportsLength",1),S([I("supportsGet() is deprecated. Use `instanceof pyodide.ffi.PyProxyWithGet` instead.")],f.prototype,"supportsGet",1),S([I("supportsSet() is deprecated. Use `instanceof pyodide.ffi.PyProxyWithSet` instead.")],f.prototype,"supportsSet",1),S([I("supportsHas() is deprecated. Use `instanceof pyodide.ffi.PyProxyWithHas` instead.")],f.prototype,"supportsHas",1),S([I("isIterable() is deprecated. Use `instanceof pyodide.ffi.PyIterable` instead.")],f.prototype,"isIterable",1),S([I("isIterator() is deprecated. Use `instanceof pyodide.ffi.PyIterator` instead.")],f.prototype,"isIterator",1),S([I("isAwaitable() is deprecated. Use `instanceof pyodide.ffi.PyAwaitable` instead.")],f.prototype,"isAwaitable",1),S([I("isBuffer() is deprecated. Use `instanceof pyodide.ffi.PyBuffer` instead.")],f.prototype,"isBuffer",1),S([I("isCallable() is deprecated. Use `instanceof pyodide.ffi.PyCallable` instead.")],f.prototype,"isCallable",1);var hr=f.prototype;Tests.Function=Function;var Pr=Object.create(Function.prototype,Object.getOwnPropertyDescriptors(hr));function qe(){}a(qe,"PyProxyFunction");qe.prototype=Pr;globalThis.PyProxyFunction=qe;var $=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<0)}};a($,"PyProxyWithLength");var me=class{get length(){let e=b(this),t;try{_check_gil(),t=_PyObject_Size(e)}catch(n){API.fatal_error(n)}return t===-1&&_pythonexc2js(),t}};a(me,"PyLengthMethods");var B=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<1)}};a(B,"PyProxyWithGet");var j=class{get(e){let t=b(this),n=Hiwire.new_value(e),o;try{_check_gil(),o=__pyproxy_getitem(t,n)}catch(s){API.fatal_error(s)}finally{Hiwire.decref(n)}if(o===0)if(_PyErr_Occurred())_pythonexc2js();else return;return Hiwire.pop_value(o)}};a(j,"PyGetItemMethods");var W=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<2)}};a(W,"PyProxyWithSet");var N=class{set(e,t){let n=b(this),o=Hiwire.new_value(e),s=Hiwire.new_value(t),i;try{_check_gil(),i=__pyproxy_setitem(n,o,s)}catch(u){API.fatal_error(u)}finally{Hiwire.decref(o),Hiwire.decref(s)}i===-1&&_pythonexc2js()}delete(e){let t=b(this),n=Hiwire.new_value(e),o;try{_check_gil(),o=__pyproxy_delitem(t,n)}catch(s){API.fatal_error(s)}finally{Hiwire.decref(n)}o===-1&&_pythonexc2js()}};a(N,"PySetItemMethods");var G=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<3)}};a(G,"PyProxyWithHas");var _e=class{has(e){let t=b(this),n=Hiwire.new_value(e),o;try{_check_gil(),o=__pyproxy_contains(t,n)}catch(s){API.fatal_error(s)}finally{Hiwire.decref(n)}return o===-1&&_pythonexc2js(),o===1}};a(_e,"PyContainsMethods");function*dt(r,e){try{for(;;){_check_gil();let t=__pyproxy_iter_next(r);if(t===0)break;yield Hiwire.pop_value(t)}}catch(t){API.fatal_error(t)}finally{Module.finalizationRegistry.unregister(e),_Py_DecRef(r)}_PyErr_Occurred()&&_pythonexc2js()}a(dt,"iter_helper");var z=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&(1<<4|1<<5))}};a(z,"PyIterable");var ge=class{[Symbol.iterator](){let e=b(this),t={},n;try{_check_gil(),n=_PyObject_GetIter(e)}catch(s){API.fatal_error(s)}n===0&&_pythonexc2js();let o=dt(n,t);return Module.finalizationRegistry.register(o,[n,void 0],t),o}};a(ge,"PyIterableMethods");async function*pt(r,e){try{for(;;){let t,n;try{if(_check_gil(),t=__pyproxy_aiter_next(r),t===0)break;n=Hiwire.pop_value(t)}catch(o){API.fatal_error(o)}try{yield await n}catch(o){if(o&&typeof o=="object"&&o.type==="StopAsyncIteration")return;throw o}finally{n.destroy()}}}finally{Module.finalizationRegistry.unregister(e),_Py_DecRef(r)}_PyErr_Occurred()&&_pythonexc2js()}a(pt,"aiter_helper");var V=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&(1<<9|1<<10))}};a(V,"PyAsyncIterable");var be=class{[Symbol.asyncIterator](){let e=b(this),t={},n;try{_check_gil(),n=_PyObject_GetAIter(e)}catch(s){API.fatal_error(s)}n===0&&_pythonexc2js();let o=pt(n,t);return Module.finalizationRegistry.register(o,[n,void 0],t),o}};a(be,"PyAsyncIterableMethods");var K=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<5)}};a(K,"PyIterator");var he=class{[Symbol.iterator](){return this}next(e=void 0){let t=Hiwire.new_value(e),n,o,s=stackSave(),i=stackAlloc(4);try{_check_gil(),n=__pyproxyGen_Send(b(this),t,i)}catch(c){API.fatal_error(c)}finally{Hiwire.decref(t)}let u=HEAPU32[(i>>2)+0];stackRestore(s),n===-1&&_pythonexc2js();let l=Hiwire.pop_value(u);return o=n===0,{done:o,value:l}}};a(he,"PyIteratorMethods");var q=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<11)}};a(q,"PyGenerator");var Pe=class{throw(e){let t=Hiwire.new_value(e),n,o,s=stackSave(),i=stackAlloc(4);try{_check_gil(),n=__pyproxyGen_throw(b(this),t,i)}catch(c){API.fatal_error(c)}finally{Hiwire.decref(t)}let u=HEAPU32[(i>>2)+0];stackRestore(s),n===-1&&_pythonexc2js();let l=Hiwire.pop_value(u);return o=n===0,{done:o,value:l}}return(e){let t=Hiwire.new_value(e),n,o,s=stackSave(),i=stackAlloc(4);try{_check_gil(),n=__pyproxyGen_return(b(this),t,i)}catch(c){API.fatal_error(c)}finally{Hiwire.decref(t)}let u=HEAPU32[(i>>2)+0];stackRestore(s),n===-1&&_pythonexc2js();let l=Hiwire.pop_value(u);return o=n===0,{done:o,value:l}}};a(Pe,"PyGeneratorMethods");var J=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<10)}};a(J,"PyAsyncIterator");var xe=class{[Symbol.asyncIterator](){return this}async next(e=void 0){let t=Hiwire.new_value(e),n;try{_check_gil(),n=__pyproxyGen_asend(b(this),t)}catch(i){API.fatal_error(i)}finally{Hiwire.decref(t)}n===0&&_pythonexc2js();let o=Hiwire.pop_value(n),s;try{s=await o}catch(i){if(i&&typeof i=="object"&&i.type==="StopAsyncIteration")return{done:!0,value:s};throw i}finally{o.destroy()}return{done:!1,value:s}}};a(xe,"PyAsyncIteratorMethods");var Y=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<12)}};a(Y,"PyAsyncGenerator");var we=class{async throw(e){let t=Hiwire.new_value(e),n;try{_check_gil(),n=__pyproxyGen_athrow(b(this),t)}catch(i){API.fatal_error(i)}finally{Hiwire.decref(t)}n===0&&_pythonexc2js();let o=Hiwire.pop_value(n),s;try{s=await o}catch(i){if(i&&typeof i=="object"){if(i.type==="StopAsyncIteration")return{done:!0,value:s};if(i.type==="GeneratorExit")return{done:!0,value:s}}throw i}finally{o.destroy()}return{done:!1,value:s}}async return(e){let t;try{_check_gil(),t=__pyproxyGen_areturn(b(this))}catch(s){API.fatal_error(s)}t===0&&_pythonexc2js();let n=Hiwire.pop_value(t),o;try{o=await n}catch(s){if(s&&typeof s=="object"){if(s.type==="StopAsyncIteration")return{done:!0,value:o};if(s.type==="GeneratorExit")return{done:!0,value:e}}throw s}finally{n.destroy()}return{done:!1,value:o}}};a(we,"PyAsyncGeneratorMethods");var X=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<13)}};a(X,"PySequence");function ft(r,e){let t=r.toString(),n=e.toString();return t===n?0:t<n?-1:1}a(ft,"defaultCompareFunc");var ve=class{get[Symbol.isConcatSpreadable](){return!0}join(e){return Array.prototype.join.call(this,e)}slice(e,t){return Array.prototype.slice.call(this,e,t)}lastIndexOf(e,t){return t===void 0&&(t=this.length),Array.prototype.lastIndexOf.call(this,e,t)}indexOf(e,t){return Array.prototype.indexOf.call(this,e,t)}forEach(e,t){Array.prototype.forEach.call(this,e,t)}map(e,t){return Array.prototype.map.call(this,e,t)}filter(e,t){return Array.prototype.filter.call(this,e,t)}some(e,t){return Array.prototype.some.call(this,e,t)}every(e,t){return Array.prototype.every.call(this,e,t)}reduce(...e){return Array.prototype.reduce.apply(this,e)}reduceRight(...e){return Array.prototype.reduceRight.apply(this,e)}at(e){return Array.prototype.at.call(this,e)}concat(...e){return Array.prototype.concat.apply(this,e)}includes(e){return this.has(e)}entries(){return Array.prototype.entries.call(this)}keys(){return Array.prototype.keys.call(this)}values(){return Array.prototype.values.call(this)}find(e,t){return Array.prototype.find.call(this,e,t)}findIndex(e,t){return Array.prototype.findIndex.call(this,e,t)}};a(ve,"PySequenceMethods");var Q=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<13)}};a(Q,"PyMutableSequence");var Ae=class{reverse(){return this.$reverse(),this}sort(e){let t=API.public_api.pyimport("functools"),n=t.cmp_to_key,o;e?o=e:o=ft;function s(u,l){return u===void 0&&l===void 0?0:u===void 0?1:l===void 0?-1:o(u,l)}a(s,"wrapper");let i;try{i=n(s),this.$sort.callKwargs({key:i})}finally{i==null||i.destroy(),n.destroy(),t.destroy()}return this}splice(e,t,...n){return t===void 0&&(t=1<<31-1),ht(this,e,e+t,n)}push(...e){for(let t of e)this.append(t);return this.length}pop(){return mr(this,!1)}shift(){return mr(this,!0)}unshift(...e){return e.forEach((t,n)=>{this.insert(n,t)}),this.length}copyWithin(...e){return Array.prototype.copyWithin.apply(this,e),this}fill(...e){return Array.prototype.fill.apply(this,e),this}};a(Ae,"PyMutableSequenceMethods");function mt(r,e){let t=b(r),n=Hiwire.new_value(e),o;try{_check_gil(),o=__pyproxy_hasattr(t,n)}catch(s){API.fatal_error(s)}finally{Hiwire.decref(n)}return o===-1&&_pythonexc2js(),o!==0}a(mt,"python_hasattr");function _t(r,e){let{shared:t}=O(r),n=Hiwire.new_value(e),o,s=t.cache.cacheId;try{_check_gil(),o=__pyproxy_getattr(t.ptr,n,s)}catch(i){API.fatal_error(i)}finally{Hiwire.decref(n)}return o===0&&_PyErr_Occurred()&&_pythonexc2js(),o}a(_t,"python_getattr");function gt(r,e,t){let n=b(r),o=Hiwire.new_value(e),s=Hiwire.new_value(t),i;try{_check_gil(),i=__pyproxy_setattr(n,o,s)}catch(u){API.fatal_error(u)}finally{Hiwire.decref(o),Hiwire.decref(s)}i===-1&&_pythonexc2js()}a(gt,"python_setattr");function bt(r,e){let t=b(r),n=Hiwire.new_value(e),o;try{_check_gil(),o=__pyproxy_delattr(t,n)}catch(s){API.fatal_error(s)}finally{Hiwire.decref(n)}o===-1&&_pythonexc2js()}a(bt,"python_delattr");function ht(r,e,t,n){let o=b(r),s=Hiwire.new_value(n),i;try{_check_gil(),i=__pyproxy_slice_assign(o,e,t,s)}catch(u){API.fatal_error(u)}finally{Hiwire.decref(s)}return i===0&&_pythonexc2js(),Hiwire.pop_value(i)}a(ht,"python_slice_assign");function mr(r,e){let t=b(r),n;try{_check_gil(),n=__pyproxy_pop(t,e)}catch(o){API.fatal_error(o)}return n===0&&_pythonexc2js(),Hiwire.pop_value(n)}a(mr,"python_pop");function fe(r,e,t){return r instanceof Function?e in r&&!["name","length","caller","arguments",t?"prototype":void 0].includes(e):e in r}a(fe,"filteredHasKey");var M={isExtensible(){return!0},has(r,e){return fe(r,e,!1)?!0:typeof e=="symbol"?!1:(e.startsWith("$")&&(e=e.slice(1)),mt(r,e))},get(r,e){if(typeof e=="symbol"||fe(r,e,!0))return Reflect.get(r,e);e.startsWith("$")&&(e=e.slice(1));let t=_t(r,e);if(t!==0)return Hiwire.pop_value(t)},set(r,e,t){let n=Object.getOwnPropertyDescriptor(r,e);return n&&!n.writable&&!n.set?!1:typeof e=="symbol"||fe(r,e,!0)?Reflect.set(r,e,t):(e.startsWith("$")&&(e=e.slice(1)),gt(r,e,t),!0)},deleteProperty(r,e){let t=Object.getOwnPropertyDescriptor(r,e);return t&&!t.configurable?!1:typeof e=="symbol"||fe(r,e,!0)?Reflect.deleteProperty(r,e):(e.startsWith("$")&&(e=e.slice(1)),bt(r,e),!0)},ownKeys(r){let e=b(r),t;try{_check_gil(),t=__pyproxy_ownKeys(e)}catch(o){API.fatal_error(o)}t===0&&_pythonexc2js();let n=Hiwire.pop_value(t);return n.push(...Reflect.ownKeys(r)),n},apply(r,e,t){return r.apply(e,t)}};function Ge(r){return r&&typeof r=="object"&&r.constructor&&r.constructor.name==="PythonError"}a(Ge,"isPythonError");var Pt={isExtensible(){return!0},has(r,e){return typeof e=="string"&&/^[0-9]*$/.test(e)?Number(e)<r.length:M.has(r,e)},get(r,e){if(e==="length")return r.length;if(typeof e=="string"&&/^[0-9]*$/.test(e))try{return j.prototype.get.call(r,Number(e))}catch(t){if(Ge(t))return;throw t}return M.get(r,e)},set(r,e,t){if(typeof e=="string"&&/^[0-9]*$/.test(e))try{return N.prototype.set.call(r,Number(e),t),!0}catch(n){if(Ge(n))return!1;throw n}return M.set(r,e,t)},deleteProperty(r,e){if(typeof e=="string"&&/^[0-9]*$/.test(e))try{return N.prototype.delete.call(r,Number(e)),!0}catch(t){if(Ge(t))return!1;throw t}return M.deleteProperty(r,e)},ownKeys(r){let e=M.ownKeys(r);return e.push(...Array.from({length:r.length},(t,n)=>n.toString())),e.push("length"),e}},Z=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<6)}};a(Z,"PyAwaitable");var Se=class{_ensure_future(){let{shared:e}=Ee(this);if(e.promise)return e.promise;let t=e.ptr;t||O(this);let n,o,s=new Promise((c,y)=>{n=c,o=y}),i=Hiwire.new_value(n),u=Hiwire.new_value(o),l;try{_check_gil(),l=__pyproxy_ensure_future(t,i,u)}catch(c){API.fatal_error(c)}finally{Hiwire.decref(u),Hiwire.decref(i)}return l===-1&&_pythonexc2js(),e.promise=s,this.destroy(),s}then(e,t){return this._ensure_future().then(e,t)}catch(e){return this._ensure_future().catch(e)}finally(e){return this._ensure_future().finally(e)}};a(Se,"PyAwaitableMethods");var ee=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<8)}};a(ee,"PyCallable");var re=class{apply(e,t){return t=function(...n){return n}.apply(void 0,t),t=pr(this,e,t),Module.callPyObject(b(this),t)}call(e,...t){return t=pr(this,e,t),Module.callPyObject(b(this),t)}callKwargs(...e){if(e.length===0)throw new TypeError("callKwargs requires at least one argument (the key word argument object)");let t=e.pop();if(t.constructor!==void 0&&t.constructor.name!=="Object")throw new TypeError("kwargs argument is not an object");return Module.callPyObjectKwargs(b(this),e,t)}bind(e,...t){let{shared:n,props:o}=O(this),{boundArgs:s,boundThis:i,isBound:u}=o,l=e;u&&(l=i);let c=s.concat(t);return o=Object.assign({},o,{boundArgs:c,isBound:!0,boundThis:l}),ke(n.ptr,{shared:n,flags:g(this),props:o})}captureThis(){let{props:e,shared:t}=O(this);return e=Object.assign({},e,{captureThis:!0}),ke(t.ptr,{shared:t,flags:g(this),props:e})}};a(re,"PyCallableMethods");re.prototype.prototype=Function.prototype;var xt=new Map([["i8",Int8Array],["u8",Uint8Array],["u8clamped",Uint8ClampedArray],["i16",Int16Array],["u16",Uint16Array],["i32",Int32Array],["u32",Uint32Array],["i32",Int32Array],["u32",Uint32Array],["i64",globalThis.BigInt64Array],["u64",globalThis.BigUint64Array],["f32",Float32Array],["f64",Float64Array],["dataview",DataView]]),F=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&!!(g(e)&1<<7)}};a(F,"PyBuffer");var Ie=class{getBuffer(e){let t;if(e&&(t=xt.get(e),t===void 0))throw new Error(`Unknown type ${e}`);let n=stackSave(),o=stackAlloc(HEAPU32[(_buffer_struct_size>>2)+0]),s=b(this),i;try{_check_gil(),i=__pyproxy_get_buffer(o,s)}catch(A){API.fatal_error(A)}i===-1&&_pythonexc2js();let u=HEAPU32[(o>>2)+0],l=HEAPU32[(o>>2)+1],c=HEAPU32[(o>>2)+2],y=!!HEAPU32[(o>>2)+3],p=HEAPU32[(o>>2)+4],_=HEAPU32[(o>>2)+5],d=Hiwire.pop_value(HEAPU32[(o>>2)+6]),m=Hiwire.pop_value(HEAPU32[(o>>2)+7]),w=HEAPU32[(o>>2)+8],T=!!HEAPU32[(o>>2)+9],Le=!!HEAPU32[(o>>2)+10],ce=UTF8ToString(p);stackRestore(n);let L=!1;try{let A=!1;t===void 0&&([t,A]=Module.processBufferFormatString(ce," In this case, you can pass an explicit type argument."));let k=parseInt(t.name.replace(/[^0-9]/g,""))/8||1;if(A&&k>1)throw new Error("Javascript has no native support for big endian buffers. In this case, you can pass an explicit type argument. For instance, `getBuffer('dataview')` will return a `DataView`which has native support for reading big endian data. Alternatively, toJs will automatically convert the buffer to little endian.");let R=c-l;if(R!==0&&(u%k!==0||l%k!==0||c%k!==0))throw new Error(`Buffer does not have valid alignment for a ${t.name}`);let $e=R/k,Gr=(u-l)/k,Be;R===0?Be=new t:Be=new t(HEAPU32.buffer,l,$e);for(let zr of m.keys())m[zr]/=k;return L=!0,Object.create(E.prototype,Object.getOwnPropertyDescriptors({offset:Gr,readonly:y,format:ce,itemsize:_,ndim:d.length,nbytes:R,shape:d,strides:m,data:Be,c_contiguous:T,f_contiguous:Le,_view_ptr:w,_released:!1}))}finally{if(!L)try{_check_gil(),_PyBuffer_Release(w),_PyMem_Free(w)}catch(A){API.fatal_error(A)}}}};a(Ie,"PyBufferMethods");var te=class extends f{static[Symbol.hasInstance](e){return API.isPyProxy(e)&&e.type==="dict"}};a(te,"PyDict");var E=class{constructor(){throw new TypeError("PyBufferView is not a constructor")}release(){if(!this._released){try{_check_gil(),_PyBuffer_Release(this._view_ptr),_PyMem_Free(this._view_ptr)}catch(e){API.fatal_error(e)}this._released=!0,this.data=null}}};a(E,"PyBufferView");var kr=Xr(Ar());function Er(r){if(typeof r=="string")r=new Error(r);else if(r&&typeof r=="object"&&r.name==="ExitStatus"){let e=r.status;r=new U(r.message),r.status=e}else if(typeof r!="object"||r===null||typeof r.stack!="string"||typeof r.message!="string"){let e=API.getTypeTag(r),t=`A value of type ${typeof r} with tag ${e} was thrown as an error!`;try{t+=`
String interpolation of the thrown value gives """${r}""".`}catch(n){t+=`
String interpolation of the thrown value fails.`}try{t+=`
The thrown value's toString method returns """${r.toString()}""".`}catch(n){t+=`
The thrown value's toString method fails.`}r=new Error(t)}return r}a(Er,"ensureCaughtObjectIsError");var ne=class extends Error{constructor(t,n,o){n||(n=`The exception is an object of type ${t} at address ${o} which does not inherit from std::exception`);super(n);this.ty=t}};a(ne,"CppException");Object.defineProperty(ne.prototype,"name",{get(){return`${this.constructor.name} ${this.ty}`}});function Fr(r){let[e,t]=Module.getExceptionMessage(r);return new ne(e,t,r)}a(Fr,"convertCppException");Tests.convertCppException=Fr;var Sr=!1;API.fatal_error=function(r){if(r&&r.pyodide_fatal_error)return;if(Sr){console.error("Recursive call to fatal_error. Inner error was:"),console.error(r);return}if(r instanceof C)throw r;typeof r=="number"?r=Fr(r):r=Er(r),r.pyodide_fatal_error=!0,Sr=!0;let e=r instanceof U;e||(console.error("Pyodide has suffered a fatal error. Please report this to the Pyodide maintainers."),console.error("The cause of the fatal error was:"),API.inTestHoist?(console.error(r.toString()),console.error(r.stack)):console.error(r));try{e||_dump_traceback();let n=`Pyodide already ${e?"exited":"fatally failed"} and can no longer be used.`;for(let o of Reflect.ownKeys(API.public_api))typeof o=="string"&&o.startsWith("_")||o==="version"||Object.defineProperty(API.public_api,o,{enumerable:!0,configurable:!0,get:()=>{throw new Error(n)}});API.on_fatal&&API.on_fatal(r)}catch(t){console.error("Another error occurred while handling the fatal error:"),console.error(t)}throw r};API.maybe_fatal_error=function(r){API._skip_unwind_fatal_error&&r==="unwind"||API.fatal_error(r)};var Qe=[];API.capture_stderr=function(){Qe=[],FS.createDevice("/dev","capture_stderr",null,r=>Qe.push(r)),FS.closeStream(2),FS.open("/dev/capture_stderr",1)};API.restore_stderr=function(){return FS.closeStream(2),FS.unlink("/dev/capture_stderr"),FS.open("/dev/stderr",1),new TextDecoder().decode(new Uint8Array(Qe))};API.fatal_loading_error=function(...r){let e=r.join(" ");if(_PyErr_Occurred()){API.capture_stderr(),_PyErr_Print();let t=API.restore_stderr();e+=`
`+t}throw new ae(e)};function Ze(r){if(!r)return!1;let e=r.fileName||"";if(e.includes("wasm-function"))return!0;if(!e.includes("pyodide.asm.js"))return!1;let t=r.functionName||"";return t.startsWith("Object.")&&(t=t.slice(7)),t in API.public_api&&t!=="PythonError"?(r.functionName=t,!1):!0}a(Ze,"isPyodideFrame");function Ir(r){return Ze(r)&&r.functionName==="new_error"}a(Ir,"isErrorStart");Module.handle_js_error=function(r){if(r&&r.pyodide_fatal_error)throw r;if(r instanceof Module._PropagatePythonError)return;let e=!1;r instanceof v&&(e=_restore_sys_last_exception(r.__error_address));let t,n;try{t=kr.default.parse(r)}catch(o){n=!0}if(n&&(r=Er(r)),!e){let o=Hiwire.new_value(r),s=_JsProxy_create(o);_set_error(s),_Py_DecRef(s),Hiwire.decref(o)}if(!n){if(Ir(t[0])||Ir(t[1]))for(;Ze(t[0]);)t.shift();for(let o of t){if(Ze(o))break;let s=stringToNewUTF8(o.functionName||"???"),i=stringToNewUTF8(o.fileName||"???.js");__PyTraceback_Add(s,i,o.lineNumber),_free(s),_free(i)}}};var v=class extends Error{constructor(t,n,o){let s=Error.stackTraceLimit;Error.stackTraceLimit=1/0;super(n);Error.stackTraceLimit=s,this.type=t,this.__error_address=o}};a(v,"PythonError");API.PythonError=v;var oe=class extends Error{constructor(){super("If you are seeing this message, an internal Pyodide error has occurred. Please report it to the Pyodide maintainers.")}};a(oe,"_PropagatePythonError");function wt(r){Object.defineProperty(r.prototype,"name",{value:r.name})}a(wt,"setName");var ae=class extends Error{};a(ae,"FatalPyodideError");var U=class extends Error{};a(U,"Exit");var C=class extends Error{};a(C,"NoGilError");[oe,ae,U,v,C].forEach(wt);API.NoGilError=C;Module._PropagatePythonError=oe;API.errorConstructors=new Map([EvalError,RangeError,ReferenceError,SyntaxError,TypeError,URIError,globalThis.DOMException,globalThis.AssertionError,globalThis.SystemError].filter(r=>r).map(r=>[r.constructor.name,r]));API.deserializeError=function(r,e,t){let n=API.errorConstructors.get(r)||Error,o=new n(e);return API.errorConstructors.has(r)||(o.name=r),o.message=e,o.stack=t,o};var Or={PyProxy:f,PyProxyWithLength:$,PyProxyWithGet:B,PyProxyWithSet:W,PyProxyWithHas:G,PyDict:te,PyIterable:z,PyAsyncIterable:V,PyIterator:K,PyAsyncIterator:J,PyGenerator:q,PyAsyncGenerator:Y,PyAwaitable:Z,PyCallable:ee,PyBuffer:F,PyBufferView:E,PythonError:v,PySequence:X,PyMutableSequence:Q};var Tr=a(r=>{r.id!=="canvas"&&console.warn("If you are using canvas element for SDL library, it should have id 'canvas' to work properly."),Module.canvas=r},"setCanvas2D"),Dr=a(()=>Module.canvas,"getCanvas2D"),vt=a(r=>{Tr(r)},"setCanvas3D"),At=a(()=>Dr(),"getCanvas3D"),Hr={setCanvas2D:Tr,getCanvas2D:Dr,setCanvas3D:vt,getCanvas3D:At};function Fe(){let r=Promise.resolve();async function e(){let t=r,n;return r=new Promise(o=>n=o),await t,n}return a(e,"acquireLock"),e}a(Fe,"createLock");function St(r,e,t){let n=r.substring(0,r.lastIndexOf("/")),o=e||[];o=o.concat(API.defaultLdLibraryPath,[n]);let s=a(l=>{for(let c of o){let y=Module.PATH.join2(c,l);if(Module.FS.findObject(y)!==null)return y}return l},"resolvePath"),i=a(l=>Module.FS.readFile(s(l)),"readFile");return t!==void 0&&(i=a(l=>t(s(l)),"readFile")),{findObject:(l,c)=>Module.FS.findObject(s(l),c),readFile:i}}a(St,"createDynlibFS");var It=Fe();async function Rr(r,e,t,n){let o=await It(),s=St(r,t,n);try{if(await Module.loadDynamicLibrary(r,{loadAsync:!0,nodelete:!0,allowUndefined:!0,global:e,fs:s}),e&&Module.PATH.isAbs(r)){let i=Module.PATH.basename(r);Module.LDSO.loadedLibsByName[i]||(Module.LDSO.loadedLibsByName[i]=Module.LDSO.loadedLibsByName[r])}}catch(i){if(i&&i.message&&i.message.includes("need to see wasm magic number")){console.warn(`Failed to load dynlib ${r}. We probably just tried to load a linux .so file or something.`);return}throw i}finally{o()}}a(Rr,"loadDynlib");async function er(r,e){let t=`${API.sitepackages}/${r.file_name.split("-")[0]}.libs`,n=yr(Module.FS.readFile),o=!!r.shared_library,s;if(o)s=e.map(i=>({path:i,global:!0}));else{let i=kt(e,n);s=e.map(u=>{let l=i.has(Module.PATH.basename(u));return{path:u,global:l||!!r.shared_library}})}s.sort((i,u)=>Number(u.global)-Number(i.global));for(let{path:i,global:u}of s)await Rr(i,u,[t],n)}a(er,"loadDynlibsFromPackage");function kt(r,e){let t=Module.FS.readFile;e!==void 0&&(t=e);let n=new Set;return r.forEach(o=>{let s=t(o);Module.getDylinkMetadata(s).neededDynlibs.forEach(u=>{n.add(u)})}),n}a(kt,"calculateGlobalLibs");API.loadDynlib=Rr;API.loadDynlibsFromPackage=er;async function Et(r){let e=await r;if(!e.packages)throw new Error("Loaded pyodide lock file does not contain the expected key 'packages'.");API.lockfile_info=e.info,API.lockfile_packages=e.packages,API.lockfile_unvendored_stdlibs_and_test=[],API.repodata_info=e.info,API.repodata_packages=e.packages,API._import_name_to_package_name=new Map;for(let t of Object.keys(API.lockfile_packages)){let n=API.lockfile_packages[t];for(let o of n.imports)API._import_name_to_package_name.set(o,t);n.package_type==="cpython_module"&&API.lockfile_unvendored_stdlibs_and_test.push(t)}API.lockfile_unvendored_stdlibs=API.lockfile_unvendored_stdlibs_and_test.filter(t=>t!=="test"),await Te(API.config.packages,{messageCallback(){}})}a(Et,"initializePackageIndex");API.lockFilePromise&&(API.packageIndexReady=Et(API.lockFilePromise));var Oe;API.setCdnUrl=function(r){Oe=r};var ie="default channel",Ft=/^.*?([^\/]*)\.whl$/;function Ot(r){let e=Ft.exec(r);if(e)return e[1].toLowerCase().split("-").slice(0,-4).join("-")}a(Ot,"_uri_to_package_name");function Mr(){let r=a(()=>{},"_resolve"),e=a(()=>{},"_reject"),t=new Promise((n,o)=>{r=n,e=o});return t.resolve=r,t.reject=e,t}a(Mr,"createDonePromise");function Nr(r,e){if(r=r.toLowerCase(),e.has(r))return;let t=API.lockfile_packages[r];if(!t)throw new Error(`No known package with name '${r}'`);if(e.set(r,{name:r,channel:ie,depends:t.depends,installPromise:void 0,done:Mr()}),H[r]===void 0)for(let n of t.depends)Nr(n,e)}a(Nr,"addPackageToLoad");function Tt(r,e){let t=new Map;for(let n of r){let o=Ot(n);if(o===void 0){Nr(n,t);continue}let s=n;if(t.has(o)&&t.get(o).channel!==s){e(`Loading same package ${o} from ${s} and ${t.get(o).channel}`);continue}t.set(o,{name:o,channel:s,depends:[],installPromise:void 0,done:Mr()})}return t}a(Tt,"recursiveDependencies");async function Dt(r,e,t=!0){let n;P?(n=API.config.packageCacheDir,await pe.mkdir(API.config.packageCacheDir,{recursive:!0})):n=API.config.indexURL;let o,s,i;if(e===ie){if(!(r in API.lockfile_packages))throw new Error(`Internal error: no entry for package named ${r}`);o=API.lockfile_packages[r].file_name,s=ye(o,n),i="sha256-"+ur(API.lockfile_packages[r].sha256)}else s=e,i=void 0;t||(i=void 0);try{return await D(s,i)}catch(l){if(!P||e!==ie)throw l}console.log(`Didn't find package ${o} locally, attempting to load from ${Oe}`);let u=await D(Oe+o);return console.log(`Package ${o} loaded from ${Oe}, caching the wheel in node_modules for future use.`),await pe.writeFile(s,u),u}a(Dt,"downloadPackage");async function Ht(r,e,t){let n=API.lockfile_packages[r];n||(n={file_name:".whl",shared_library:!1,depends:[],imports:[],install_dir:"site"});let o=n.file_name,s=API.package_loader.unpack_buffer.callKwargs({buffer:e,filename:o,target:n.install_dir,calculate_dynlibs:!0,installer:"pyodide.loadPackage",source:t===ie?"pyodide":t});await er(n,s)}a(Ht,"installPackage");async function Rt(r,e,t,n,o=!0){if(H[r]!==void 0)return;let s=e.get(r);try{let i=await Dt(s.name,s.channel,o),u=s.depends.map(l=>e.has(l)?e.get(l).done:Promise.resolve());await API.bootstrapFinalizedPromise,await Promise.all(u),await Ht(s.name,i,s.channel),t.add(s.name),H[s.name]=s.channel}catch(i){n.set(r,i)}finally{s.done.resolve()}}a(Rt,"downloadAndInstall");var Mt=Fe(),Dn=dr(`Passing a messageCallback (resp. errorCallback) as the second (resp. third) argument to loadPackage is deprecated and will be removed in v0.24. Instead use:
   { messageCallback : callbackFunc }`);async function Te(r,e={checkIntegrity:!0}){let t=e.messageCallback||console.log,n=e.errorCallback||console.error;r instanceof f&&(r=r.toJs()),Array.isArray(r)||(r=[r]);let o=Tt(r,n);for(let[c,y]of o){let p=H[c];p!==void 0&&(o.delete(c),p===y.channel||y.channel===ie?t(`${c} already loaded from ${p}`):n(`URI mismatch, attempting to load package ${c} from ${y.channel} while it is already loaded from ${p}. To override a dependency, load the custom package first.`))}if(o.size===0){t("No new packages to load");return}let s=[...o.keys()].join(", "),i=new Set,u=new Map,l=await Mt();try{t(`Loading ${s}`);for(let[c]of o){if(H[c]){o.delete(c);continue}o.get(c).installPromise=Rt(c,o,i,u,e.checkIntegrity)}if(await Promise.all(Array.from(o.values()).map(({installPromise:c})=>c)),Module.reportUndefinedSymbols(),i.size>0){let c=Array.from(i).join(", ");t(`Loaded ${c}`)}if(u.size>0){let c=Array.from(u.keys()).join(", ");t(`Failed to load ${c}`);for(let[y,p]of u)n(`The following error occurred while loading ${y}:`),n(p.message)}API.importlib.invalidate_caches()}finally{l()}}a(Te,"loadPackage");var H={};var Ur="0.25.0.dev0";var nr=P?je("fs"):void 0,Br=P?je("tty"):void 0;function jr(r){try{nr.fsyncSync(r)}catch(e){if(e&&e.code==="EINVAL")return;throw e}}a(jr,"nodeFsync");var Wr=!1,Ue={},x={};function tr(r){Ue[x.stdin]=r}a(tr,"_setStdinOps");function Nt(r){Ue[x.stdout]=r}a(Nt,"_setStdoutOps");function Ut(r){Ue[x.stderr]=r}a(Ut,"_setStderrOps");function Ct(r){return r&&typeof r=="object"&&"errno"in r}a(Ct,"isErrnoError");var Lt=new Int32Array(new WebAssembly.Memory({shared:!0,initial:1,maximum:1}).buffer);function $t(r){try{return Atomics.wait(Lt,0,0,r),!0}catch(e){return!1}}a($t,"syncSleep");function Bt(r){for(;;)try{return r()}catch(e){if(e&&e.code==="EAGAIN"&&$t(100))continue;throw e}}a(Bt,"handleEAGAIN");function Cr(r,e,t){let n;try{n=Bt(e)}catch(o){throw o&&o.code&&Module.ERRNO_CODES[o.code]?new FS.ErrnoError(Module.ERRNO_CODES[o.code]):Ct(o)?o:(console.error("Error thrown in read:"),console.error(o),new FS.ErrnoError(29))}if(n===void 0)throw console.warn(`${t} returned undefined; a correct implementation must return a number`),new FS.ErrnoError(29);return n!==0&&(r.node.timestamp=Date.now()),n}a(Cr,"readWriteHelper");var Lr=a((r,e,t)=>API.typedArrayAsUint8Array(r).subarray(e,e+t),"prepareBuffer"),rr={open:function(r){let e=Ue[r.node.rdev];if(!e)throw new FS.ErrnoError(43);r.devops=e,r.tty=r.devops.isatty,r.seekable=!1},close:function(r){r.stream_ops.fsync(r)},fsync:function(r){let e=r.devops;e.fsync&&e.fsync()},read:function(r,e,t,n,o){return e=Lr(e,t,n),Cr(r,()=>r.devops.read(e),"read")},write:function(r,e,t,n,o){return e=Lr(e,t,n),Cr(r,()=>r.devops.write(e),"write")}};function Ce(){Wr&&(FS.closeStream(0),FS.closeStream(1),FS.closeStream(2),FS.open("/dev/stdin",0),FS.open("/dev/stdout",1),FS.open("/dev/stderr",1))}a(Ce,"refreshStreams");API.initializeStreams=function(r,e,t){let n=FS.createDevice.major++;x.stdin=FS.makedev(n,0),x.stdout=FS.makedev(n,1),x.stderr=FS.makedev(n,2),FS.registerDevice(x.stdin,rr),FS.registerDevice(x.stdout,rr),FS.registerDevice(x.stderr,rr),FS.unlink("/dev/stdin"),FS.unlink("/dev/stdout"),FS.unlink("/dev/stderr"),FS.mkdev("/dev/stdin",x.stdin),FS.mkdev("/dev/stdout",x.stdout),FS.mkdev("/dev/stderr",x.stderr),le({stdin:r}),ar({batched:e}),ir({batched:t}),Wr=!0,Ce()};function jt(){le(P?new He(process.stdin.fd):{stdin:()=>prompt()})}a(jt,"setDefaultStdin");function Wt(){tr(new De),Ce()}a(Wt,"setStdinError");function le(r={}){let{stdin:e,error:t,isatty:n,autoEOF:o,read:s}=r,i=+!!e+ +!!t+ +!!s;if(i>1)throw new TypeError("At most one of stdin, read, and error must be provided.");if(!e&&o!==void 0)throw new TypeError("The 'autoEOF' option can only be used with the 'stdin' option");if(i===0){jt();return}t&&Wt(),e&&(o=o===void 0?!0:o,tr(new Re(e.bind(r),!!n,o))),s&&tr(r),Ce()}a(le,"setStdin");function or(r,e,t){let n=r.raw,o=r.write,s=r.isatty,i=r.batched,u=+!!n+ +!!i+ +!!o;if(u>1)throw new TypeError("At most one of 'raw', 'batched', and 'write' must be passed");if(u===0)return or(t(),e,t);if(!n&&!o&&s)throw new TypeError("Cannot set 'isatty' to true unless 'raw' or 'write' is provided");n&&e(new Me(n.bind(r),!!s)),i&&e(new Ne(i.bind(r))),o&&e(r),Ce()}a(or,"_setStdwrite");function Gt(){return P?new se(process.stdout.fd):{batched:r=>console.log(r)}}a(Gt,"_getStdoutDefaults");function zt(){return P?new se(process.stderr.fd):{batched:r=>console.warn(r)}}a(zt,"_getStderrDefaults");function ar(r={}){or(r,Nt,Gt)}a(ar,"setStdout");function ir(r={}){or(r,Ut,zt)}a(ir,"setStderr");var Vt=new TextEncoder,$r=new TextDecoder,De=class{read(e){throw new FS.ErrnoError(29)}};a(De,"ErrorReader");var He=class{constructor(e){this.fd=e,this.isatty=Br.isatty(e)}read(e){try{return nr.readSync(this.fd,e)}catch(t){if(t.toString().includes("EOF"))return 0;throw t}}fsync(){jr(this.fd)}};a(He,"NodeReader");var Re=class{constructor(e,t,n){this.infunc=e,this.isatty=t,this.autoEOF=n,this.index=0,this.saved=void 0,this.insertEOF=!1}_getInput(){if(this.saved)return this.saved;let e=this.infunc();if(typeof e=="number")return e;if(e){if(ArrayBuffer.isView(e)){if(e.BYTES_PER_ELEMENT!==1)throw console.warn(`Expected BYTES_PER_ELEMENT to be 1, infunc gave ${e.constructor}`),new FS.ErrnoError(29);return e}if(typeof e=="string")return e.endsWith(`
`)||(e+=`
`),e;if(Object.prototype.toString.call(e)==="[object ArrayBuffer]")return new Uint8Array(e);throw console.warn("Expected result to be undefined, null, string, array buffer, or array buffer view"),new FS.ErrnoError(29)}}read(e){if(this.insertEOF)return this.insertEOF=!1,0;let t=0;for(;;){let n=this._getInput();if(typeof n=="number"){e[0]=n,e=e.subarray(1),t++;continue}let o;if(n&&n.length>0)if(typeof n=="string"){let{read:s,written:i}=Vt.encodeInto(n,e);this.saved=n.slice(s),t+=i,o=e[i-1],e=e.subarray(i)}else{let s;n.length>e.length?(e.set(n.subarray(0,e.length)),this.saved=n.subarray(e.length),s=e.length):(e.set(n),this.saved=void 0,s=n.length),t+=s,o=e[s-1],e=e.subarray(s)}if(!(n&&n.length>0)||this.autoEOF||e.length===0)return this.insertEOF=t>0&&this.autoEOF&&o!==10,t}}fsync(){}};a(Re,"LegacyReader");var Me=class{constructor(e,t){this.out=e,this.isatty=t}write(e){for(let t of e)this.out(t);return e.length}};a(Me,"CharacterCodeWriter");var Ne=class{constructor(e){this.isatty=!1;this.out=e,this.output=[]}write(e){for(let t of e)t===10?(this.out($r.decode(new Uint8Array(this.output))),this.output=[]):t!==0&&this.output.push(t);return e.length}fsync(){this.output&&this.output.length>0&&(this.out($r.decode(new Uint8Array(this.output))),this.output=[])}};a(Ne,"StringWriter");var se=class{constructor(e){this.fd=e,this.isatty=Br.isatty(e)}write(e){return nr.writeSync(this.fd,e)}fsync(){jr(this.fd)}};a(se,"NodeWriter");API.loadBinaryFile=D;API.loadBinaryFile=D;API.rawRun=a(function(e){let t=Module.stringToNewUTF8(e);Module.API.capture_stderr();let n=_PyRun_SimpleString(t);_free(t);let o=Module.API.restore_stderr().trim();return[n,o]},"rawRun");API.runPythonInternal=function(r){return API._pyodide._base.eval_code(r,API.runPythonInternal_dict)};API.saveState=()=>API.pyodide_py._state.save_state();API.restoreState=r=>API.pyodide_py._state.restore_state(r);var h=class{static async loadPackagesFromImports(e,t={checkIntegrity:!0}){let n=API.pyodide_code.find_imports(e),o;try{o=n.toJs()}finally{n.destroy()}if(o.length===0)return;let s=API._import_name_to_package_name,i=new Set;for(let u of o)s.has(u)&&i.add(s.get(u));i.size&&await Te(Array.from(i),t)}static runPython(e,t={}){return t.globals||(t.globals=API.globals),API.pyodide_code.eval_code.callKwargs(e,t)}static async runPythonAsync(e,t={}){return t.globals||(t.globals=API.globals),await API.pyodide_code.eval_code_async.callKwargs(e,t)}static registerJsModule(e,t){API.pyodide_ffi.register_js_module(e,t)}static unregisterJsModule(e){API.pyodide_ffi.unregister_js_module(e)}static toPy(e,{depth:t,defaultConverter:n}={depth:-1}){switch(typeof e){case"string":case"number":case"boolean":case"bigint":case"undefined":return e}if(!e||API.isPyProxy(e))return e;let o=0,s=0,i=0;try{o=Hiwire.new_value(e);try{s=Module.js2python_convert(o,{depth:t,defaultConverter:n})}catch(u){throw u instanceof Module._PropagatePythonError&&_pythonexc2js(),u}if(_JsProxy_Check(s))return e;i=_python2js(s),i===0&&_pythonexc2js()}finally{Hiwire.decref(o),_Py_DecRef(s)}return Hiwire.pop_value(i)}static pyimport(e){return API.importlib.import_module(e)}static unpackArchive(e,t,n={}){if(!ArrayBuffer.isView(e)&&API.getTypeTag(e)!=="[object ArrayBuffer]")throw new TypeError("Expected argument 'buffer' to be an ArrayBuffer or an ArrayBuffer view");API.typedArrayAsUint8Array(e);let o=n.extractDir;API.package_loader.unpack_buffer.callKwargs({buffer:e,format:t,extract_dir:o,installer:"pyodide.unpackArchive"})}static async mountNativeFS(e,t){if(t.constructor.name!=="FileSystemDirectoryHandle")throw new TypeError("Expected argument 'fileSystemHandle' to be a FileSystemDirectoryHandle");return Module.FS.findObject(e)==null&&Module.FS.mkdirTree(e),Module.FS.mount(Module.FS.filesystems.NATIVEFS_ASYNC,{fileSystemHandle:t},e),await new Promise((n,o)=>Module.FS.syncfs(!0,n)),{syncfs:async()=>new Promise((n,o)=>Module.FS.syncfs(!1,n))}}static registerComlink(e){API._Comlink=e}static setInterruptBuffer(e){Module.HEAP8[Module._Py_EMSCRIPTEN_SIGNAL_HANDLING]=!!e,Module.Py_EmscriptenSignalBuffer=e}static checkInterrupt(){if(_PyGILState_Check()){__PyErr_CheckSignals()&&_pythonexc2js();return}else{let e=Module.Py_EmscriptenSignalBuffer;if(e&&e[0]===2)throw new Module.FS.ErrnoError(27)}}static isPyProxy(e){return console.warn("pyodide.isPyProxy() is deprecated. Use `instanceof pyodide.ffi.PyProxy` instead."),this.isPyProxy=API.isPyProxy,API.isPyProxy(e)}static get PyBuffer(){return console.warn("pyodide.PyBuffer is deprecated. Use `pyodide.ffi.PyBufferView` instead."),Object.defineProperty(this,"PyBuffer",{value:E}),E}static get PyProxyBuffer(){return console.warn("pyodide.PyProxyBuffer is deprecated. Use `pyodide.ffi.PyBuffer` instead."),Object.defineProperty(this,"PyProxyBuffer",{value:F}),F}static get PythonError(){return console.warn("pyodide.PythonError is deprecated. Use `pyodide.ffi.PythonError` instead."),Object.defineProperty(this,"PythonError",{value:v}),v}static setDebug(e){let t=!!API.debug_ffi;return API.debug_ffi=e,t}};a(h,"PyodideAPI"),h.version=Ur,h.loadPackage=Te,h.loadedPackages=H,h.ffi=Or,h.setStdin=le,h.setStdout=ar,h.setStderr=ir,h.globals={},h.FS={},h.PATH={},h.canvas=Hr,h.ERRNO_CODES={},h.pyodide_py={};API.makePublicAPI=function(){let r=Object.getOwnPropertyDescriptors(h);delete r.prototype;let e=Object.create({},r);return API.public_api=e,e.FS=Module.FS,e.PATH=Module.PATH,e.ERRNO_CODES=Module.ERRNO_CODES,e._module=Module,e._api=API,e};})();
//# sourceMappingURL=_pyodide.out.js.map
}
const API = Module.API;
const Hiwire = {};
const Tests = {};
API.tests = Tests;
API.version = "0.25.0.dev0";
Module.hiwire = Hiwire;
function getTypeTag(x) {
  try {
    return Object.prototype.toString.call(x);
  } catch (e) {
    return "";
  }
}
API.getTypeTag = getTypeTag;

/**
 * Safe property check
 *
 * Observe whether a property exists or not without invoking getters.
 * Should never throw as long as arguments have the correct types.
 *
 * obj: an object
 * prop: a string or symbol
 */
function hasProperty(obj, prop) {
  try {
    while (obj) {
      if (Object.getOwnPropertyDescriptor(obj, prop)) {
        return true;
      }
      obj = Object.getPrototypeOf(obj);
    }
  } catch (e) {}
  return false;
}

/**
 * Observe whether a method exists or not
 *
 * Invokes getters but catches any error produced by a getter and throws it away.
 * Never throws an error
 *
 * obj: an object
 * prop: a string or symbol
 */
function hasMethod(obj, prop) {
  try {
    return typeof obj[prop] === "function";
  } catch (e) {
    return false;
  }
}

const pyproxyIsAlive = (px) => !!Module.PyProxy_getAttrsQuiet(px).shared.ptr;
API.pyproxyIsAlive = pyproxyIsAlive;

const errNoRet = () => {
  throw new Error(
    "Assertion error: control reached end of function without return",
  );
};

Module.reportUndefinedSymbols = () => {};
pyodide_js_init();

function __hiwire_deduplicate_new() { return new Map(); }
function __hiwire_deduplicate_get(map,value) { return map.get(value); }
function __hiwire_deduplicate_set(map,value,ref) { map.set(value, ref); }
function __hiwire_deduplicate_delete(map,value) { map.delete(value); }
function unbox_small_structs(type_ptr) { var type_id = HEAPU16[(type_ptr + 6 >> 1) + 0]; while (type_id === 13) { var elements = HEAPU32[(type_ptr + 8 >> 2) + 0]; var first_element = HEAPU32[(elements >> 2) + 0]; if (first_element === 0) { type_id = 0; break; } else if (HEAPU32[(elements >> 2) + 1] === 0) { type_ptr = first_element; type_id = HEAPU16[(first_element + 6 >> 1) + 0]; } else { break; } } return [type_ptr, type_id]; }
function ffi_call_js(cif,fn,rvalue,avalue) { var abi = HEAPU32[(cif >> 2) + 0]; var nargs = HEAPU32[(cif >> 2) + 1]; var nfixedargs = HEAPU32[(cif >> 2) + 6]; var arg_types_ptr = HEAPU32[(cif >> 2) + 2]; var rtype_unboxed = unbox_small_structs(HEAPU32[(cif >> 2) + 3]); var rtype_ptr = rtype_unboxed[0]; var rtype_id = rtype_unboxed[1]; var orig_stack_ptr = stackSave(); var cur_stack_ptr = orig_stack_ptr; var args = []; var ret_by_arg = false; if (rtype_id === 15) { throw new Error('complex ret marshalling nyi'); } if (rtype_id < 0 || rtype_id > 15) { throw new Error('Unexpected rtype ' + rtype_id); } if (rtype_id === 4 || rtype_id === 13) { args.push(rvalue); ret_by_arg = true; } ; for (var i = 0; i < nfixedargs; i++) { var arg_ptr = HEAPU32[(avalue >> 2) + i]; var arg_unboxed = unbox_small_structs(HEAPU32[(arg_types_ptr >> 2) + i]); var arg_type_ptr = arg_unboxed[0]; var arg_type_id = arg_unboxed[1]; switch (arg_type_id) { case 1: case 10: case 9: case 14: args.push(HEAPU32[(arg_ptr >> 2) + 0]); ; break; case 2: args.push(HEAPF32[(arg_ptr >> 2) + 0]); ; break; case 3: args.push(HEAPF64[(arg_ptr >> 3) + 0]); ; break; case 5: args.push(HEAPU8[arg_ptr + 0]); ; break; case 6: args.push(HEAP8[arg_ptr + 0]); ; break; case 7: args.push(HEAPU16[(arg_ptr >> 1) + 0]); ; break; case 8: args.push(HEAP16[(arg_ptr >> 1) + 0]); ; break; case 11: case 12: args.push(HEAPU64[(arg_ptr >> 3) + 0]); ; break; case 4: args.push(HEAPU64[(arg_ptr >> 3) + 0]); args.push(HEAPU64[(arg_ptr >> 3) + 1]); ; break; case 13: var size = HEAPU32[(arg_type_ptr >> 2) + 0]; var align = HEAPU16[(arg_type_ptr + 4 >> 1) + 0]; ((cur_stack_ptr -= (size)), (cur_stack_ptr &= (~((align) - 1)))); HEAP8.subarray(cur_stack_ptr, cur_stack_ptr+size).set(HEAP8.subarray(arg_ptr, arg_ptr + size)); args.push(cur_stack_ptr); ; break; case 15: throw new Error('complex marshalling nyi'); default: throw new Error('Unexpected type ' + arg_type_id); } } if (nfixedargs != nargs) { ; var struct_arg_info = []; for (var i = nargs - 1; i >= nfixedargs; i--) { var arg_ptr = HEAPU32[(avalue >> 2) + i]; var arg_unboxed = unbox_small_structs(HEAPU32[(arg_types_ptr >> 2) + i]); var arg_type_ptr = arg_unboxed[0]; var arg_type_id = arg_unboxed[1]; switch (arg_type_id) { case 5: case 6: ((cur_stack_ptr -= (1)), (cur_stack_ptr &= (~((1) - 1)))); HEAPU8[cur_stack_ptr + 0] = HEAPU8[arg_ptr + 0]; break; case 7: case 8: ((cur_stack_ptr -= (2)), (cur_stack_ptr &= (~((2) - 1)))); HEAPU16[(cur_stack_ptr >> 1) + 0] = HEAPU16[(arg_ptr >> 1) + 0]; break; case 1: case 9: case 10: case 14: case 2: ((cur_stack_ptr -= (4)), (cur_stack_ptr &= (~((4) - 1)))); HEAPU32[(cur_stack_ptr >> 2) + 0] = HEAPU32[(arg_ptr >> 2) + 0]; break; case 3: case 11: case 12: ((cur_stack_ptr -= (8)), (cur_stack_ptr &= (~((8) - 1)))); HEAPU32[(cur_stack_ptr >> 2) + 0] = HEAPU32[(arg_ptr >> 2) + 0]; HEAPU32[(cur_stack_ptr >> 2) + 1] = HEAPU32[(arg_ptr >> 2) + 1]; break; case 4: ((cur_stack_ptr -= (16)), (cur_stack_ptr &= (~((8) - 1)))); HEAPU32[(cur_stack_ptr >> 2) + 0] = HEAPU32[(arg_ptr >> 2) + 0]; HEAPU32[(cur_stack_ptr >> 2) + 1] = HEAPU32[(arg_ptr >> 2) + 1]; HEAPU32[(cur_stack_ptr >> 2) + 2] = HEAPU32[(arg_ptr >> 2) + 2]; HEAPU32[(cur_stack_ptr >> 2) + 3] = HEAPU32[(arg_ptr >> 2) + 3]; break; case 13: ((cur_stack_ptr -= (4)), (cur_stack_ptr &= (~((4) - 1)))); struct_arg_info.push([cur_stack_ptr, arg_ptr, HEAPU32[(arg_type_ptr >> 2) + 0], HEAPU16[(arg_type_ptr + 4 >> 1) + 0]]); break; case 15: throw new Error('complex arg marshalling nyi'); default: throw new Error('Unexpected argtype ' + arg_type_id); } } args.push(cur_stack_ptr); for (var i = 0; i < struct_arg_info.length; i++) { var struct_info = struct_arg_info[i]; var arg_target = struct_info[0]; var arg_ptr = struct_info[1]; var size = struct_info[2]; var align = struct_info[3]; ((cur_stack_ptr -= (size)), (cur_stack_ptr &= (~((align) - 1)))); HEAP8.subarray(cur_stack_ptr, cur_stack_ptr+size).set(HEAP8.subarray(arg_ptr, arg_ptr + size)); HEAPU32[(arg_target >> 2) + 0] = cur_stack_ptr; } } stackRestore(cur_stack_ptr); stackAlloc(0); var result = (0, getWasmTableEntry(fn).apply(null, args)); stackRestore(orig_stack_ptr); if (ret_by_arg) { return; } switch (rtype_id) { case 0: break; case 1: case 9: case 10: case 14: HEAPU32[(rvalue >> 2) + 0] = result; break; case 2: HEAPF32[(rvalue >> 2) + 0] = result; break; case 3: HEAPF64[(rvalue >> 3) + 0] = result; break; case 5: case 6: HEAPU8[rvalue + 0] = result; break; case 7: case 8: HEAPU16[(rvalue >> 1) + 0] = result; break; case 11: case 12: HEAPU64[(rvalue >> 3) + 0] = result; break; case 15: throw new Error('complex ret marshalling nyi'); default: throw new Error('Unexpected rtype ' + rtype_id); } }
function ffi_closure_alloc_js(size,code) { var closure = _malloc(size); var index = getEmptyTableSlot(); HEAPU32[(code >> 2) + 0] = index; HEAPU32[(closure >> 2) + 0] = index; return closure; }
function ffi_closure_free_js(closure) { var index = HEAPU32[(closure >> 2) + 0]; freeTableIndexes.push(index); _free(closure); }
function ffi_prep_closure_loc_js(closure,cif,fun,user_data,codeloc) { var abi = HEAPU32[(cif >> 2) + 0]; var nargs = HEAPU32[(cif >> 2) + 1]; var nfixedargs = HEAPU32[(cif >> 2) + 6]; var arg_types_ptr = HEAPU32[(cif >> 2) + 2]; var rtype_unboxed = unbox_small_structs(HEAPU32[(cif >> 2) + 3]); var rtype_ptr = rtype_unboxed[0]; var rtype_id = rtype_unboxed[1]; var sig; var ret_by_arg = false; switch (rtype_id) { case 0: sig = 'v'; break; case 13: case 4: sig = 'vi'; ret_by_arg = true; break; case 1: case 5: case 6: case 7: case 8: case 9: case 10: case 14: sig = 'i'; break; case 2: sig = 'f'; break; case 3: sig = 'd'; break; case 11: case 12: sig = 'j'; break; case 15: throw new Error('complex ret marshalling nyi'); default: throw new Error('Unexpected rtype ' + rtype_id); } var unboxed_arg_type_id_list = []; var unboxed_arg_type_info_list = []; for (var i = 0; i < nargs; i++) { var arg_unboxed = unbox_small_structs(HEAPU32[(arg_types_ptr >> 2) + i]); var arg_type_ptr = arg_unboxed[0]; var arg_type_id = arg_unboxed[1]; unboxed_arg_type_id_list.push(arg_type_id); unboxed_arg_type_info_list.push([HEAPU32[(arg_type_ptr >> 2) + 0], HEAPU16[(arg_type_ptr + 4 >> 1) + 0]]); } for (var i = 0; i < nfixedargs; i++) { switch (unboxed_arg_type_id_list[i]) { case 1: case 5: case 6: case 7: case 8: case 9: case 10: case 14: case 13: sig += 'i'; break; case 2: sig += 'f'; break; case 3: sig += 'd'; break; case 4: sig += 'jj'; break; case 11: case 12: sig += 'j'; break; case 15: throw new Error('complex marshalling nyi'); default: throw new Error('Unexpected argtype ' + arg_type_id); } } if (nfixedargs < nargs) { sig += "i"; } 0; function trampoline() { var args = Array.prototype.slice.call(arguments); var size = 0; var orig_stack_ptr = stackSave(); var cur_ptr = orig_stack_ptr; var ret_ptr; var jsarg_idx = 0; if (ret_by_arg) { ret_ptr = args[jsarg_idx++]; } else { ((cur_ptr -= (8)), (cur_ptr &= (~((8) - 1)))); ret_ptr = cur_ptr; } cur_ptr -= 4 * nargs; var args_ptr = cur_ptr; var carg_idx = 0; for (; carg_idx < nfixedargs; carg_idx++) { var cur_arg = args[jsarg_idx++]; var arg_type_info = unboxed_arg_type_info_list[carg_idx]; var arg_size = arg_type_info[0]; var arg_align = arg_type_info[1]; var arg_type_id = unboxed_arg_type_id_list[carg_idx]; switch (arg_type_id) { case 5: case 6: ((cur_ptr -= (1)), (cur_ptr &= (~((4) - 1)))); HEAPU32[(args_ptr >> 2) + carg_idx] = cur_ptr; HEAPU8[cur_ptr + 0] = cur_arg; break; case 7: case 8: ((cur_ptr -= (2)), (cur_ptr &= (~((4) - 1)))); HEAPU32[(args_ptr >> 2) + carg_idx] = cur_ptr; HEAPU16[(cur_ptr >> 1) + 0] = cur_arg; break; case 1: case 9: case 10: case 14: ((cur_ptr -= (4)), (cur_ptr &= (~((4) - 1)))); HEAPU32[(args_ptr >> 2) + carg_idx] = cur_ptr; HEAPU32[(cur_ptr >> 2) + 0] = cur_arg; break; case 13: ((cur_ptr -= (arg_size)), (cur_ptr &= (~((arg_align) - 1)))); HEAP8.subarray(cur_ptr, cur_ptr + arg_size).set(HEAP8.subarray(cur_arg, cur_arg + arg_size)); HEAPU32[(args_ptr >> 2) + carg_idx] = cur_ptr; break; case 2: ((cur_ptr -= (4)), (cur_ptr &= (~((4) - 1)))); HEAPU32[(args_ptr >> 2) + carg_idx] = cur_ptr; HEAPF32[(cur_ptr >> 2) + 0] = cur_arg; break; case 3: ((cur_ptr -= (8)), (cur_ptr &= (~((8) - 1)))); HEAPU32[(args_ptr >> 2) + carg_idx] = cur_ptr; HEAPF64[(cur_ptr >> 3) + 0] = cur_arg; break; case 11: case 12: ((cur_ptr -= (8)), (cur_ptr &= (~((8) - 1)))); HEAPU32[(args_ptr >> 2) + carg_idx] = cur_ptr; HEAPU64[(cur_ptr >> 3) + 0] = cur_arg; break; case 4: ((cur_ptr -= (16)), (cur_ptr &= (~((8) - 1)))); HEAPU32[(args_ptr >> 2) + carg_idx] = cur_ptr; HEAPU64[(cur_ptr >> 3) + 0] = cur_arg; cur_arg = args[jsarg_idx++]; HEAPU64[(cur_ptr >> 3) + 1] = cur_arg; break; } } var varargs = args[args.length - 1]; for (; carg_idx < nargs; carg_idx++) { var arg_type_id = unboxed_arg_type_id_list[carg_idx]; var arg_type_info = unboxed_arg_type_info_list[carg_idx]; var arg_size = arg_type_info[0]; var arg_align = arg_type_info[1]; if (arg_type_id === 13) { var struct_ptr = HEAPU32[(varargs >> 2) + 0]; ((cur_ptr -= (arg_size)), (cur_ptr &= (~((arg_align) - 1)))); HEAP8.subarray(cur_ptr, cur_ptr + arg_size).set(HEAP8.subarray(struct_ptr, struct_ptr + arg_size)); HEAPU32[(args_ptr >> 2) + carg_idx] = cur_ptr; } else { HEAPU32[(args_ptr >> 2) + carg_idx] = varargs; } varargs += 4; } stackRestore(cur_ptr); stackAlloc(0); 0; getWasmTableEntry(HEAPU32[(closure >> 2) + 2])( HEAPU32[(closure >> 2) + 1], ret_ptr, args_ptr, HEAPU32[(closure >> 2) + 3] ); stackRestore(orig_stack_ptr); if (!ret_by_arg) { switch (sig[0]) { case "i": return HEAPU32[(ret_ptr >> 2) + 0]; case "j": return HEAPU64[(ret_ptr >> 3) + 0]; case "d": return HEAPF64[(ret_ptr >> 3) + 0]; case "f": return HEAPF32[(ret_ptr >> 2) + 0]; } } } try { var wasm_trampoline = convertJsFunctionToWasm(trampoline, sig); } catch(e) { return 1; } setWasmTableEntry(codeloc, wasm_trampoline); HEAPU32[(closure >> 2) + 1] = cif; HEAPU32[(closure >> 2) + 2] = fun; HEAPU32[(closure >> 2) + 3] = user_data; return 0; }


// end include: preamble.js

  /** @constructor */
  function ExitStatus(status) {
      this.name = 'ExitStatus';
      this.message = `Program terminated with exit(${status})`;
      this.status = status;
    }

  var callRuntimeCallbacks = (callbacks) => {
      while (callbacks.length > 0) {
        // Pass the module as the first argument.
        callbacks.shift()(Module);
      }
    };

  
    /**
     * @param {number} ptr
     * @param {string} type
     */
  function getValue(ptr, type = 'i8') {
    if (type.endsWith('*')) type = '*';
    switch (type) {
      case 'i1': return HEAP8[((ptr)>>0)];
      case 'i8': return HEAP8[((ptr)>>0)];
      case 'i16': return HEAP16[((ptr)>>1)];
      case 'i32': return HEAP32[((ptr)>>2)];
      case 'i64': return HEAP64[((ptr)>>3)];
      case 'float': return HEAPF32[((ptr)>>2)];
      case 'double': return HEAPF64[((ptr)>>3)];
      case '*': return HEAPU32[((ptr)>>2)];
      default: abort(`invalid type for getValue: ${type}`);
    }
  }

  var ptrToString = (ptr) => {
      assert(typeof ptr === 'number');
      // With CAN_ADDRESS_2GB or MEMORY64, pointers are already unsigned.
      ptr >>>= 0;
      return '0x' + ptr.toString(16).padStart(8, '0');
    };

  
    /**
     * @param {number} ptr
     * @param {number} value
     * @param {string} type
     */
  function setValue(ptr, value, type = 'i8') {
    if (type.endsWith('*')) type = '*';
    switch (type) {
      case 'i1': HEAP8[((ptr)>>0)] = value; break;
      case 'i8': HEAP8[((ptr)>>0)] = value; break;
      case 'i16': HEAP16[((ptr)>>1)] = value; break;
      case 'i32': HEAP32[((ptr)>>2)] = value; break;
      case 'i64': HEAP64[((ptr)>>3)] = BigInt(value); break;
      case 'float': HEAPF32[((ptr)>>2)] = value; break;
      case 'double': HEAPF64[((ptr)>>3)] = value; break;
      case '*': HEAPU32[((ptr)>>2)] = value; break;
      default: abort(`invalid type for setValue: ${type}`);
    }
  }

  var warnOnce = (text) => {
      if (!warnOnce.shown) warnOnce.shown = {};
      if (!warnOnce.shown[text]) {
        warnOnce.shown[text] = 1;
        if (ENVIRONMENT_IS_NODE) text = 'warning: ' + text;
        err(text);
      }
    };

  var UTF8Decoder = typeof TextDecoder != 'undefined' ? new TextDecoder('utf8') : undefined;
  
    /**
     * Given a pointer 'idx' to a null-terminated UTF8-encoded string in the given
     * array that contains uint8 values, returns a copy of that string as a
     * Javascript String object.
     * heapOrArray is either a regular array, or a JavaScript typed array view.
     * @param {number} idx
     * @param {number=} maxBytesToRead
     * @return {string}
     */
  var UTF8ArrayToString = (heapOrArray, idx, maxBytesToRead) => {
      var endIdx = idx + maxBytesToRead;
      var endPtr = idx;
      // TextDecoder needs to know the byte length in advance, it doesn't stop on
      // null terminator by itself.  Also, use the length info to avoid running tiny
      // strings through TextDecoder, since .subarray() allocates garbage.
      // (As a tiny code save trick, compare endPtr against endIdx using a negation,
      // so that undefined means Infinity)
      while (heapOrArray[endPtr] && !(endPtr >= endIdx)) ++endPtr;
  
      if (endPtr - idx > 16 && heapOrArray.buffer && UTF8Decoder) {
        return UTF8Decoder.decode(heapOrArray.subarray(idx, endPtr));
      }
      var str = '';
      // If building with TextDecoder, we have already computed the string length
      // above, so test loop end condition against that
      while (idx < endPtr) {
        // For UTF8 byte structure, see:
        // http://en.wikipedia.org/wiki/UTF-8#Description
        // https://www.ietf.org/rfc/rfc2279.txt
        // https://tools.ietf.org/html/rfc3629
        var u0 = heapOrArray[idx++];
        if (!(u0 & 0x80)) { str += String.fromCharCode(u0); continue; }
        var u1 = heapOrArray[idx++] & 63;
        if ((u0 & 0xE0) == 0xC0) { str += String.fromCharCode(((u0 & 31) << 6) | u1); continue; }
        var u2 = heapOrArray[idx++] & 63;
        if ((u0 & 0xF0) == 0xE0) {
          u0 = ((u0 & 15) << 12) | (u1 << 6) | u2;
        } else {
          if ((u0 & 0xF8) != 0xF0) warnOnce('Invalid UTF-8 leading byte ' + ptrToString(u0) + ' encountered when deserializing a UTF-8 string in wasm memory to a JS string!');
          u0 = ((u0 & 7) << 18) | (u1 << 12) | (u2 << 6) | (heapOrArray[idx++] & 63);
        }
  
        if (u0 < 0x10000) {
          str += String.fromCharCode(u0);
        } else {
          var ch = u0 - 0x10000;
          str += String.fromCharCode(0xD800 | (ch >> 10), 0xDC00 | (ch & 0x3FF));
        }
      }
      return str;
    };
  
    /**
     * Given a pointer 'ptr' to a null-terminated UTF8-encoded string in the
     * emscripten HEAP, returns a copy of that string as a Javascript String object.
     *
     * @param {number} ptr
     * @param {number=} maxBytesToRead - An optional length that specifies the
     *   maximum number of bytes to read. You can omit this parameter to scan the
     *   string until the first 0 byte. If maxBytesToRead is passed, and the string
     *   at [ptr, ptr+maxBytesToReadr[ contains a null byte in the middle, then the
     *   string will cut short at that byte index (i.e. maxBytesToRead will not
     *   produce a string of exact length [ptr, ptr+maxBytesToRead[) N.B. mixing
     *   frequent uses of UTF8ToString() with and without maxBytesToRead may throw
     *   JS JIT optimizations off, so it is worth to consider consistently using one
     * @return {string}
     */
  var UTF8ToString = (ptr, maxBytesToRead) => {
      assert(typeof ptr == 'number');
      return ptr ? UTF8ArrayToString(HEAPU8, ptr, maxBytesToRead) : '';
    };
  var ___assert_fail = (condition, filename, line, func) => {
      abort(`Assertion failed: ${UTF8ToString(condition)}, at: ` + [filename ? UTF8ToString(filename) : 'unknown filename', line, func ? UTF8ToString(func) : 'unknown function']);
    };

  var wasmTableMirror = [];
  var getWasmTableEntry = (funcPtr) => {
      var func = wasmTableMirror[funcPtr];
      if (!func) {
        if (funcPtr >= wasmTableMirror.length) wasmTableMirror.length = funcPtr + 1;
        wasmTableMirror[funcPtr] = func = wasmTable.get(funcPtr);
      }
      assert(wasmTable.get(funcPtr) == func, "JavaScript-side Wasm function table mirror is out of date!");
      return func;
    };
  var ___call_sighandler = (fp, sig) => getWasmTableEntry(fp)(sig);

  var ___emscripten_atomics_sleep = (ms) => {
      try {
        Atomics.wait(waitBuffer, 0, 0, ms);
        return 1;
      } catch (_) {
        return 0;
      }
    };

  var PATH = {
  isAbs:(path) => path.charAt(0) === '/',
  splitPath:(filename) => {
        var splitPathRe = /^(\/?|)([\s\S]*?)((?:\.{1,2}|[^\/]+?|)(\.[^.\/]*|))(?:[\/]*)$/;
        return splitPathRe.exec(filename).slice(1);
      },
  normalizeArray:(parts, allowAboveRoot) => {
        // if the path tries to go above the root, `up` ends up > 0
        var up = 0;
        for (var i = parts.length - 1; i >= 0; i--) {
          var last = parts[i];
          if (last === '.') {
            parts.splice(i, 1);
          } else if (last === '..') {
            parts.splice(i, 1);
            up++;
          } else if (up) {
            parts.splice(i, 1);
            up--;
          }
        }
        // if the path is allowed to go above the root, restore leading ..s
        if (allowAboveRoot) {
          for (; up; up--) {
            parts.unshift('..');
          }
        }
        return parts;
      },
  normalize:(path) => {
        var isAbsolute = PATH.isAbs(path),
            trailingSlash = path.substr(-1) === '/';
        // Normalize the path
        path = PATH.normalizeArray(path.split('/').filter((p) => !!p), !isAbsolute).join('/');
        if (!path && !isAbsolute) {
          path = '.';
        }
        if (path && trailingSlash) {
          path += '/';
        }
        return (isAbsolute ? '/' : '') + path;
      },
  dirname:(path) => {
        var result = PATH.splitPath(path),
            root = result[0],
            dir = result[1];
        if (!root && !dir) {
          // No dirname whatsoever
          return '.';
        }
        if (dir) {
          // It has a dirname, strip trailing slash
          dir = dir.substr(0, dir.length - 1);
        }
        return root + dir;
      },
  basename:(path) => {
        // EMSCRIPTEN return '/'' for '/', not an empty string
        if (path === '/') return '/';
        path = PATH.normalize(path);
        path = path.replace(/\/$/, "");
        var lastSlash = path.lastIndexOf('/');
        if (lastSlash === -1) return path;
        return path.substr(lastSlash+1);
      },
  join:function() {
        var paths = Array.prototype.slice.call(arguments);
        return PATH.normalize(paths.join('/'));
      },
  join2:(l, r) => {
        return PATH.normalize(l + '/' + r);
      },
  };
  
  var initRandomFill = () => {
      if (typeof crypto == 'object' && typeof crypto['getRandomValues'] == 'function') {
        // for modern web browsers
        return (view) => crypto.getRandomValues(view);
      } else
      if (ENVIRONMENT_IS_NODE) {
        // for nodejs with or without crypto support included
        try {
          var crypto_module = require('crypto');
          var randomFillSync = crypto_module['randomFillSync'];
          if (randomFillSync) {
            // nodejs with LTS crypto support
            return (view) => crypto_module['randomFillSync'](view);
          }
          // very old nodejs with the original crypto API
          var randomBytes = crypto_module['randomBytes'];
          return (view) => (
            view.set(randomBytes(view.byteLength)),
            // Return the original view to match modern native implementations.
            view
          );
        } catch (e) {
          // nodejs doesn't have crypto support
        }
      }
      // we couldn't find a proper implementation, as Math.random() is not suitable for /dev/random, see emscripten-core/emscripten/pull/7096
      abort("no cryptographic support found for randomDevice. consider polyfilling it if you want to use something insecure like Math.random(), e.g. put this in a --pre-js: var crypto = { getRandomValues: (array) => { for (var i = 0; i < array.length; i++) array[i] = (Math.random()*256)|0 } };");
    };
  var randomFill = (view) => {
      // Lazily init on the first invocation.
      return (randomFill = initRandomFill())(view);
    };
  
  
  
  var PATH_FS = {
  resolve:function() {
        var resolvedPath = '',
          resolvedAbsolute = false;
        for (var i = arguments.length - 1; i >= -1 && !resolvedAbsolute; i--) {
          var path = (i >= 0) ? arguments[i] : FS.cwd();
          // Skip empty and invalid entries
          if (typeof path != 'string') {
            throw new TypeError('Arguments to path.resolve must be strings');
          } else if (!path) {
            return ''; // an invalid portion invalidates the whole thing
          }
          resolvedPath = path + '/' + resolvedPath;
          resolvedAbsolute = PATH.isAbs(path);
        }
        // At this point the path should be resolved to a full absolute path, but
        // handle relative paths to be safe (might happen when process.cwd() fails)
        resolvedPath = PATH.normalizeArray(resolvedPath.split('/').filter((p) => !!p), !resolvedAbsolute).join('/');
        return ((resolvedAbsolute ? '/' : '') + resolvedPath) || '.';
      },
  relative:(from, to) => {
        from = PATH_FS.resolve(from).substr(1);
        to = PATH_FS.resolve(to).substr(1);
        function trim(arr) {
          var start = 0;
          for (; start < arr.length; start++) {
            if (arr[start] !== '') break;
          }
          var end = arr.length - 1;
          for (; end >= 0; end--) {
            if (arr[end] !== '') break;
          }
          if (start > end) return [];
          return arr.slice(start, end - start + 1);
        }
        var fromParts = trim(from.split('/'));
        var toParts = trim(to.split('/'));
        var length = Math.min(fromParts.length, toParts.length);
        var samePartsLength = length;
        for (var i = 0; i < length; i++) {
          if (fromParts[i] !== toParts[i]) {
            samePartsLength = i;
            break;
          }
        }
        var outputParts = [];
        for (var i = samePartsLength; i < fromParts.length; i++) {
          outputParts.push('..');
        }
        outputParts = outputParts.concat(toParts.slice(samePartsLength));
        return outputParts.join('/');
      },
  };
  
  
  
  var FS_stdin_getChar_buffer = [];
  
  var lengthBytesUTF8 = (str) => {
      var len = 0;
      for (var i = 0; i < str.length; ++i) {
        // Gotcha: charCodeAt returns a 16-bit word that is a UTF-16 encoded code
        // unit, not a Unicode code point of the character! So decode
        // UTF16->UTF32->UTF8.
        // See http://unicode.org/faq/utf_bom.html#utf16-3
        var c = str.charCodeAt(i); // possibly a lead surrogate
        if (c <= 0x7F) {
          len++;
        } else if (c <= 0x7FF) {
          len += 2;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
          len += 4; ++i;
        } else {
          len += 3;
        }
      }
      return len;
    };
  
  var stringToUTF8Array = (str, heap, outIdx, maxBytesToWrite) => {
      assert(typeof str === 'string');
      // Parameter maxBytesToWrite is not optional. Negative values, 0, null,
      // undefined and false each don't write out any bytes.
      if (!(maxBytesToWrite > 0))
        return 0;
  
      var startIdx = outIdx;
      var endIdx = outIdx + maxBytesToWrite - 1; // -1 for string null terminator.
      for (var i = 0; i < str.length; ++i) {
        // Gotcha: charCodeAt returns a 16-bit word that is a UTF-16 encoded code
        // unit, not a Unicode code point of the character! So decode
        // UTF16->UTF32->UTF8.
        // See http://unicode.org/faq/utf_bom.html#utf16-3
        // For UTF8 byte structure, see http://en.wikipedia.org/wiki/UTF-8#Description
        // and https://www.ietf.org/rfc/rfc2279.txt
        // and https://tools.ietf.org/html/rfc3629
        var u = str.charCodeAt(i); // possibly a lead surrogate
        if (u >= 0xD800 && u <= 0xDFFF) {
          var u1 = str.charCodeAt(++i);
          u = 0x10000 + ((u & 0x3FF) << 10) | (u1 & 0x3FF);
        }
        if (u <= 0x7F) {
          if (outIdx >= endIdx) break;
          heap[outIdx++] = u;
        } else if (u <= 0x7FF) {
          if (outIdx + 1 >= endIdx) break;
          heap[outIdx++] = 0xC0 | (u >> 6);
          heap[outIdx++] = 0x80 | (u & 63);
        } else if (u <= 0xFFFF) {
          if (outIdx + 2 >= endIdx) break;
          heap[outIdx++] = 0xE0 | (u >> 12);
          heap[outIdx++] = 0x80 | ((u >> 6) & 63);
          heap[outIdx++] = 0x80 | (u & 63);
        } else {
          if (outIdx + 3 >= endIdx) break;
          if (u > 0x10FFFF) warnOnce('Invalid Unicode code point ' + ptrToString(u) + ' encountered when serializing a JS string to a UTF-8 string in wasm memory! (Valid unicode code points should be in range 0-0x10FFFF).');
          heap[outIdx++] = 0xF0 | (u >> 18);
          heap[outIdx++] = 0x80 | ((u >> 12) & 63);
          heap[outIdx++] = 0x80 | ((u >> 6) & 63);
          heap[outIdx++] = 0x80 | (u & 63);
        }
      }
      // Null-terminate the pointer to the buffer.
      heap[outIdx] = 0;
      return outIdx - startIdx;
    };
  /** @type {function(string, boolean=, number=)} */
  function intArrayFromString(stringy, dontAddNull, length) {
    var len = length > 0 ? length : lengthBytesUTF8(stringy)+1;
    var u8array = new Array(len);
    var numBytesWritten = stringToUTF8Array(stringy, u8array, 0, u8array.length);
    if (dontAddNull) u8array.length = numBytesWritten;
    return u8array;
  }
  var FS_stdin_getChar = () => {
      if (!FS_stdin_getChar_buffer.length) {
        var result = null;
        if (ENVIRONMENT_IS_NODE) {
          // we will read data by chunks of BUFSIZE
          var BUFSIZE = 256;
          var buf = Buffer.alloc(BUFSIZE);
          var bytesRead = 0;
  
          // For some reason we must suppress a closure warning here, even though
          // fd definitely exists on process.stdin, and is even the proper way to
          // get the fd of stdin,
          // https://github.com/nodejs/help/issues/2136#issuecomment-523649904
          // This started to happen after moving this logic out of library_tty.js,
          // so it is related to the surrounding code in some unclear manner.
          /** @suppress {missingProperties} */
          var fd = process.stdin.fd;
  
          try {
            bytesRead = fs.readSync(fd, buf);
          } catch(e) {
            // Cross-platform differences: on Windows, reading EOF throws an exception, but on other OSes,
            // reading EOF returns 0. Uniformize behavior by treating the EOF exception to return 0.
            if (e.toString().includes('EOF')) bytesRead = 0;
            else throw e;
          }
  
          if (bytesRead > 0) {
            result = buf.slice(0, bytesRead).toString('utf-8');
          } else {
            result = null;
          }
        } else
        if (typeof window != 'undefined' &&
          typeof window.prompt == 'function') {
          // Browser.
          result = window.prompt('Input: ');  // returns null on cancel
          if (result !== null) {
            result += '\n';
          }
        } else if (typeof readline == 'function') {
          // Command line.
          result = readline();
          if (result !== null) {
            result += '\n';
          }
        }
        if (!result) {
          return null;
        }
        FS_stdin_getChar_buffer = intArrayFromString(result, true);
      }
      return FS_stdin_getChar_buffer.shift();
    };
  var TTY = {
  ttys:[],
  init() {
        // https://github.com/emscripten-core/emscripten/pull/1555
        // if (ENVIRONMENT_IS_NODE) {
        //   // currently, FS.init does not distinguish if process.stdin is a file or TTY
        //   // device, it always assumes it's a TTY device. because of this, we're forcing
        //   // process.stdin to UTF8 encoding to at least make stdin reading compatible
        //   // with text files until FS.init can be refactored.
        //   process.stdin.setEncoding('utf8');
        // }
      },
  shutdown() {
        // https://github.com/emscripten-core/emscripten/pull/1555
        // if (ENVIRONMENT_IS_NODE) {
        //   // inolen: any idea as to why node -e 'process.stdin.read()' wouldn't exit immediately (with process.stdin being a tty)?
        //   // isaacs: because now it's reading from the stream, you've expressed interest in it, so that read() kicks off a _read() which creates a ReadReq operation
        //   // inolen: I thought read() in that case was a synchronous operation that just grabbed some amount of buffered data if it exists?
        //   // isaacs: it is. but it also triggers a _read() call, which calls readStart() on the handle
        //   // isaacs: do process.stdin.pause() and i'd think it'd probably close the pending call
        //   process.stdin.pause();
        // }
      },
  register(dev, ops) {
        TTY.ttys[dev] = { input: [], output: [], ops: ops };
        FS.registerDevice(dev, TTY.stream_ops);
      },
  stream_ops:{
  open(stream) {
          var tty = TTY.ttys[stream.node.rdev];
          if (!tty) {
            throw new FS.ErrnoError(43);
          }
          stream.tty = tty;
          stream.seekable = false;
        },
  close(stream) {
          // flush any pending line data
          stream.tty.ops.fsync(stream.tty);
        },
  fsync(stream) {
          stream.tty.ops.fsync(stream.tty);
        },
  read(stream, buffer, offset, length, pos /* ignored */) {
          if (!stream.tty || !stream.tty.ops.get_char) {
            throw new FS.ErrnoError(60);
          }
          var bytesRead = 0;
          for (var i = 0; i < length; i++) {
            var result;
            try {
              result = stream.tty.ops.get_char(stream.tty);
            } catch (e) {
              throw new FS.ErrnoError(29);
            }
            if (result === undefined && bytesRead === 0) {
              throw new FS.ErrnoError(6);
            }
            if (result === null || result === undefined) break;
            bytesRead++;
            buffer[offset+i] = result;
          }
          if (bytesRead) {
            stream.node.timestamp = Date.now();
          }
          return bytesRead;
        },
  write(stream, buffer, offset, length, pos) {
          if (!stream.tty || !stream.tty.ops.put_char) {
            throw new FS.ErrnoError(60);
          }
          try {
            for (var i = 0; i < length; i++) {
              stream.tty.ops.put_char(stream.tty, buffer[offset+i]);
            }
          } catch (e) {
            throw new FS.ErrnoError(29);
          }
          if (length) {
            stream.node.timestamp = Date.now();
          }
          return i;
        },
  },
  default_tty_ops:{
  get_char(tty) {
          return FS_stdin_getChar();
        },
  put_char(tty, val) {
          if (val === null || val === 10) {
            out(UTF8ArrayToString(tty.output, 0));
            tty.output = [];
          } else {
            if (val != 0) tty.output.push(val); // val == 0 would cut text output off in the middle.
          }
        },
  fsync(tty) {
          if (tty.output && tty.output.length > 0) {
            out(UTF8ArrayToString(tty.output, 0));
            tty.output = [];
          }
        },
  ioctl_tcgets(tty) {
          // typical setting
          return {
            c_iflag: 25856,
            c_oflag: 5,
            c_cflag: 191,
            c_lflag: 35387,
            c_cc: [
              0x03, 0x1c, 0x7f, 0x15, 0x04, 0x00, 0x01, 0x00, 0x11, 0x13, 0x1a, 0x00,
              0x12, 0x0f, 0x17, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            ]
          };
        },
  ioctl_tcsets(tty, optional_actions, data) {
          // currently just ignore
          return 0;
        },
  ioctl_tiocgwinsz(tty) {
          return [24, 80];
        },
  },
  default_tty1_ops:{
  put_char(tty, val) {
          if (val === null || val === 10) {
            err(UTF8ArrayToString(tty.output, 0));
            tty.output = [];
          } else {
            if (val != 0) tty.output.push(val);
          }
        },
  fsync(tty) {
          if (tty.output && tty.output.length > 0) {
            err(UTF8ArrayToString(tty.output, 0));
            tty.output = [];
          }
        },
  },
  };
  
  
  var zeroMemory = (address, size) => {
      HEAPU8.fill(0, address, address + size);
      return address;
    };
  
  var alignMemory = (size, alignment) => {
      assert(alignment, "alignment argument is required");
      return Math.ceil(size / alignment) * alignment;
    };
  var mmapAlloc = (size) => {
      size = alignMemory(size, 65536);
      var ptr = _emscripten_builtin_memalign(65536, size);
      if (!ptr) return 0;
      return zeroMemory(ptr, size);
    };
  var MEMFS = {
  ops_table:null,
  mount(mount) {
        return MEMFS.createNode(null, '/', 16384 | 511 /* 0777 */, 0);
      },
  createNode(parent, name, mode, dev) {
        if (FS.isBlkdev(mode) || FS.isFIFO(mode)) {
          // no supported
          throw new FS.ErrnoError(63);
        }
        if (!MEMFS.ops_table) {
          MEMFS.ops_table = {
            dir: {
              node: {
                getattr: MEMFS.node_ops.getattr,
                setattr: MEMFS.node_ops.setattr,
                lookup: MEMFS.node_ops.lookup,
                mknod: MEMFS.node_ops.mknod,
                rename: MEMFS.node_ops.rename,
                unlink: MEMFS.node_ops.unlink,
                rmdir: MEMFS.node_ops.rmdir,
                readdir: MEMFS.node_ops.readdir,
                symlink: MEMFS.node_ops.symlink
              },
              stream: {
                llseek: MEMFS.stream_ops.llseek
              }
            },
            file: {
              node: {
                getattr: MEMFS.node_ops.getattr,
                setattr: MEMFS.node_ops.setattr
              },
              stream: {
                llseek: MEMFS.stream_ops.llseek,
                read: MEMFS.stream_ops.read,
                write: MEMFS.stream_ops.write,
                allocate: MEMFS.stream_ops.allocate,
                mmap: MEMFS.stream_ops.mmap,
                msync: MEMFS.stream_ops.msync
              }
            },
            link: {
              node: {
                getattr: MEMFS.node_ops.getattr,
                setattr: MEMFS.node_ops.setattr,
                readlink: MEMFS.node_ops.readlink
              },
              stream: {}
            },
            chrdev: {
              node: {
                getattr: MEMFS.node_ops.getattr,
                setattr: MEMFS.node_ops.setattr
              },
              stream: FS.chrdev_stream_ops
            }
          };
        }
        var node = FS.createNode(parent, name, mode, dev);
        if (FS.isDir(node.mode)) {
          node.node_ops = MEMFS.ops_table.dir.node;
          node.stream_ops = MEMFS.ops_table.dir.stream;
          node.contents = {};
        } else if (FS.isFile(node.mode)) {
          node.node_ops = MEMFS.ops_table.file.node;
          node.stream_ops = MEMFS.ops_table.file.stream;
          node.usedBytes = 0; // The actual number of bytes used in the typed array, as opposed to contents.length which gives the whole capacity.
          // When the byte data of the file is populated, this will point to either a typed array, or a normal JS array. Typed arrays are preferred
          // for performance, and used by default. However, typed arrays are not resizable like normal JS arrays are, so there is a small disk size
          // penalty involved for appending file writes that continuously grow a file similar to std::vector capacity vs used -scheme.
          node.contents = null; 
        } else if (FS.isLink(node.mode)) {
          node.node_ops = MEMFS.ops_table.link.node;
          node.stream_ops = MEMFS.ops_table.link.stream;
        } else if (FS.isChrdev(node.mode)) {
          node.node_ops = MEMFS.ops_table.chrdev.node;
          node.stream_ops = MEMFS.ops_table.chrdev.stream;
        }
        node.timestamp = Date.now();
        // add the new node to the parent
        if (parent) {
          parent.contents[name] = node;
          parent.timestamp = node.timestamp;
        }
        return node;
      },
  getFileDataAsTypedArray(node) {
        if (!node.contents) return new Uint8Array(0);
        if (node.contents.subarray) return node.contents.subarray(0, node.usedBytes); // Make sure to not return excess unused bytes.
        return new Uint8Array(node.contents);
      },
  expandFileStorage(node, newCapacity) {
        var prevCapacity = node.contents ? node.contents.length : 0;
        if (prevCapacity >= newCapacity) return; // No need to expand, the storage was already large enough.
        // Don't expand strictly to the given requested limit if it's only a very small increase, but instead geometrically grow capacity.
        // For small filesizes (<1MB), perform size*2 geometric increase, but for large sizes, do a much more conservative size*1.125 increase to
        // avoid overshooting the allocation cap by a very large margin.
        var CAPACITY_DOUBLING_MAX = 1024 * 1024;
        newCapacity = Math.max(newCapacity, (prevCapacity * (prevCapacity < CAPACITY_DOUBLING_MAX ? 2.0 : 1.125)) >>> 0);
        if (prevCapacity != 0) newCapacity = Math.max(newCapacity, 256); // At minimum allocate 256b for each file when expanding.
        var oldContents = node.contents;
        node.contents = new Uint8Array(newCapacity); // Allocate new storage.
        if (node.usedBytes > 0) node.contents.set(oldContents.subarray(0, node.usedBytes), 0); // Copy old data over to the new storage.
      },
  resizeFileStorage(node, newSize) {
        if (node.usedBytes == newSize) return;
        if (newSize == 0) {
          node.contents = null; // Fully decommit when requesting a resize to zero.
          node.usedBytes = 0;
        } else {
          var oldContents = node.contents;
          node.contents = new Uint8Array(newSize); // Allocate new storage.
          if (oldContents) {
            node.contents.set(oldContents.subarray(0, Math.min(newSize, node.usedBytes))); // Copy old data over to the new storage.
          }
          node.usedBytes = newSize;
        }
      },
  node_ops:{
  getattr(node) {
          var attr = {};
          // device numbers reuse inode numbers.
          attr.dev = FS.isChrdev(node.mode) ? node.id : 1;
          attr.ino = node.id;
          attr.mode = node.mode;
          attr.nlink = 1;
          attr.uid = 0;
          attr.gid = 0;
          attr.rdev = node.rdev;
          if (FS.isDir(node.mode)) {
            attr.size = 4096;
          } else if (FS.isFile(node.mode)) {
            attr.size = node.usedBytes;
          } else if (FS.isLink(node.mode)) {
            attr.size = node.link.length;
          } else {
            attr.size = 0;
          }
          attr.atime = new Date(node.timestamp);
          attr.mtime = new Date(node.timestamp);
          attr.ctime = new Date(node.timestamp);
          // NOTE: In our implementation, st_blocks = Math.ceil(st_size/st_blksize),
          //       but this is not required by the standard.
          attr.blksize = 4096;
          attr.blocks = Math.ceil(attr.size / attr.blksize);
          return attr;
        },
  setattr(node, attr) {
          if (attr.mode !== undefined) {
            node.mode = attr.mode;
          }
          if (attr.timestamp !== undefined) {
            node.timestamp = attr.timestamp;
          }
          if (attr.size !== undefined) {
            MEMFS.resizeFileStorage(node, attr.size);
          }
        },
  lookup(parent, name) {
          throw FS.genericErrors[44];
        },
  mknod(parent, name, mode, dev) {
          return MEMFS.createNode(parent, name, mode, dev);
        },
  rename(old_node, new_dir, new_name) {
          // if we're overwriting a directory at new_name, make sure it's empty.
          if (FS.isDir(old_node.mode)) {
            var new_node;
            try {
              new_node = FS.lookupNode(new_dir, new_name);
            } catch (e) {
            }
            if (new_node) {
              for (var i in new_node.contents) {
                throw new FS.ErrnoError(55);
              }
            }
          }
          // do the internal rewiring
          delete old_node.parent.contents[old_node.name];
          old_node.parent.timestamp = Date.now()
          old_node.name = new_name;
          new_dir.contents[new_name] = old_node;
          new_dir.timestamp = old_node.parent.timestamp;
          old_node.parent = new_dir;
        },
  unlink(parent, name) {
          delete parent.contents[name];
          parent.timestamp = Date.now();
        },
  rmdir(parent, name) {
          var node = FS.lookupNode(parent, name);
          for (var i in node.contents) {
            throw new FS.ErrnoError(55);
          }
          delete parent.contents[name];
          parent.timestamp = Date.now();
        },
  readdir(node) {
          var entries = ['.', '..'];
          for (var key in node.contents) {
            if (!node.contents.hasOwnProperty(key)) {
              continue;
            }
            entries.push(key);
          }
          return entries;
        },
  symlink(parent, newname, oldpath) {
          var node = MEMFS.createNode(parent, newname, 511 /* 0777 */ | 40960, 0);
          node.link = oldpath;
          return node;
        },
  readlink(node) {
          if (!FS.isLink(node.mode)) {
            throw new FS.ErrnoError(28);
          }
          return node.link;
        },
  },
  stream_ops:{
  read(stream, buffer, offset, length, position) {
          var contents = stream.node.contents;
          if (position >= stream.node.usedBytes) return 0;
          var size = Math.min(stream.node.usedBytes - position, length);
          assert(size >= 0);
          if (size > 8 && contents.subarray) { // non-trivial, and typed array
            buffer.set(contents.subarray(position, position + size), offset);
          } else {
            for (var i = 0; i < size; i++) buffer[offset + i] = contents[position + i];
          }
          return size;
        },
  write(stream, buffer, offset, length, position, canOwn) {
          // The data buffer should be a typed array view
          assert(!(buffer instanceof ArrayBuffer));
  
          if (!length) return 0;
          var node = stream.node;
          node.timestamp = Date.now();
  
          if (buffer.subarray && (!node.contents || node.contents.subarray)) { // This write is from a typed array to a typed array?
            if (canOwn) {
              assert(position === 0, 'canOwn must imply no weird position inside the file');
              node.contents = buffer.subarray(offset, offset + length);
              node.usedBytes = length;
              return length;
            } else if (node.usedBytes === 0 && position === 0) { // If this is a simple first write to an empty file, do a fast set since we don't need to care about old data.
              node.contents = buffer.slice(offset, offset + length);
              node.usedBytes = length;
              return length;
            } else if (position + length <= node.usedBytes) { // Writing to an already allocated and used subrange of the file?
              node.contents.set(buffer.subarray(offset, offset + length), position);
              return length;
            }
          }
  
          // Appending to an existing file and we need to reallocate, or source data did not come as a typed array.
          MEMFS.expandFileStorage(node, position+length);
          if (node.contents.subarray && buffer.subarray) {
            // Use typed array write which is available.
            node.contents.set(buffer.subarray(offset, offset + length), position);
          } else {
            for (var i = 0; i < length; i++) {
             node.contents[position + i] = buffer[offset + i]; // Or fall back to manual write if not.
            }
          }
          node.usedBytes = Math.max(node.usedBytes, position + length);
          return length;
        },
  llseek(stream, offset, whence) {
          var position = offset;
          if (whence === 1) {
            position += stream.position;
          } else if (whence === 2) {
            if (FS.isFile(stream.node.mode)) {
              position += stream.node.usedBytes;
            }
          }
          if (position < 0) {
            throw new FS.ErrnoError(28);
          }
          return position;
        },
  allocate(stream, offset, length) {
          MEMFS.expandFileStorage(stream.node, offset + length);
          stream.node.usedBytes = Math.max(stream.node.usedBytes, offset + length);
        },
  mmap(stream, length, position, prot, flags) {
          if (!FS.isFile(stream.node.mode)) {
            throw new FS.ErrnoError(43);
          }
          var ptr;
          var allocated;
          var contents = stream.node.contents;
          // Only make a new copy when MAP_PRIVATE is specified.
          if (!(flags & 2) && contents.buffer === HEAP8.buffer) {
            // We can't emulate MAP_SHARED when the file is not backed by the
            // buffer we're mapping to (e.g. the HEAP buffer).
            allocated = false;
            ptr = contents.byteOffset;
          } else {
            // Try to avoid unnecessary slices.
            if (position > 0 || position + length < contents.length) {
              if (contents.subarray) {
                contents = contents.subarray(position, position + length);
              } else {
                contents = Array.prototype.slice.call(contents, position, position + length);
              }
            }
            allocated = true;
            ptr = mmapAlloc(length);
            if (!ptr) {
              throw new FS.ErrnoError(48);
            }
            HEAP8.set(contents, ptr);
          }
          return { ptr, allocated };
        },
  msync(stream, buffer, offset, length, mmapFlags) {
          MEMFS.stream_ops.write(stream, buffer, 0, length, offset, false);
          // should we check if bytesWritten and length are the same?
          return 0;
        },
  },
  };
  
  /** @param {boolean=} noRunDep */
  var asyncLoad = (url, onload, onerror, noRunDep) => {
      var dep = !noRunDep ? getUniqueRunDependency(`al ${url}`) : '';
      readAsync(url, (arrayBuffer) => {
        assert(arrayBuffer, `Loading data file "${url}" failed (no arrayBuffer).`);
        onload(new Uint8Array(arrayBuffer));
        if (dep) removeRunDependency(dep);
      }, (event) => {
        if (onerror) {
          onerror();
        } else {
          throw `Loading data file "${url}" failed.`;
        }
      });
      if (dep) addRunDependency(dep);
    };
  
  
  var preloadPlugins = Module['preloadPlugins'] || [];
  var FS_handledByPreloadPlugin = (byteArray, fullname, finish, onerror) => {
      // Ensure plugins are ready.
      if (typeof Browser != 'undefined') Browser.init();
  
      var handled = false;
      preloadPlugins.forEach((plugin) => {
        if (handled) return;
        if (plugin['canHandle'](fullname)) {
          plugin['handle'](byteArray, fullname, finish, onerror);
          handled = true;
        }
      });
      return handled;
    };
  var FS_createPreloadedFile = (parent, name, url, canRead, canWrite, onload, onerror, dontCreateFile, canOwn, preFinish) => {
      // TODO we should allow people to just pass in a complete filename instead
      // of parent and name being that we just join them anyways
      var fullname = name ? PATH_FS.resolve(PATH.join2(parent, name)) : parent;
      var dep = getUniqueRunDependency(`cp ${fullname}`); // might have several active requests for the same fullname
      function processData(byteArray) {
        function finish(byteArray) {
          if (preFinish) preFinish();
          if (!dontCreateFile) {
            FS.createDataFile(parent, name, byteArray, canRead, canWrite, canOwn);
          }
          if (onload) onload();
          removeRunDependency(dep);
        }
        if (FS_handledByPreloadPlugin(byteArray, fullname, finish, () => {
          if (onerror) onerror();
          removeRunDependency(dep);
        })) {
          return;
        }
        finish(byteArray);
      }
      addRunDependency(dep);
      if (typeof url == 'string') {
        asyncLoad(url, (byteArray) => processData(byteArray), onerror);
      } else {
        processData(url);
      }
    };
  
  var FS_modeStringToFlags = (str) => {
      var flagModes = {
        'r': 0,
        'r+': 2,
        'w': 512 | 64 | 1,
        'w+': 512 | 64 | 2,
        'a': 1024 | 64 | 1,
        'a+': 1024 | 64 | 2,
      };
      var flags = flagModes[str];
      if (typeof flags == 'undefined') {
        throw new Error(`Unknown file open mode: ${str}`);
      }
      return flags;
    };
  
  var FS_getMode = (canRead, canWrite) => {
      var mode = 0;
      if (canRead) mode |= 292 | 73;
      if (canWrite) mode |= 146;
      return mode;
    };
  
  
  
  
  
  
  var ERRNO_CODES = {
  };
  
  var NODEFS = {
  isWindows:false,
  staticInit() {
        NODEFS.isWindows = !!process.platform.match(/^win/);
        var flags = process.binding("constants");
        // Node.js 4 compatibility: it has no namespaces for constants
        if (flags["fs"]) {
          flags = flags["fs"];
        }
        NODEFS.flagsForNodeMap = {
          "1024": flags["O_APPEND"],
          "64": flags["O_CREAT"],
          "128": flags["O_EXCL"],
          "256": flags["O_NOCTTY"],
          "0": flags["O_RDONLY"],
          "2": flags["O_RDWR"],
          "4096": flags["O_SYNC"],
          "512": flags["O_TRUNC"],
          "1": flags["O_WRONLY"],
          "131072": flags["O_NOFOLLOW"],
        };
        // The 0 define must match on both sides, as otherwise we would not
        // know to add it.
        assert(NODEFS.flagsForNodeMap["0"] === 0);
      },
  convertNodeCode(e) {
        var code = e.code;
        assert(code in ERRNO_CODES, `unexpected node error code: ${code} (${e})`);
        return ERRNO_CODES[code];
      },
  mount(mount) {
        assert(ENVIRONMENT_IS_NODE);
        return NODEFS.createNode(null, '/', NODEFS.getMode(mount.opts.root), 0);
      },
  createNode(parent, name, mode, dev) {
        if (!FS.isDir(mode) && !FS.isFile(mode) && !FS.isLink(mode)) {
          throw new FS.ErrnoError(28);
        }
        var node = FS.createNode(parent, name, mode);
        node.node_ops = NODEFS.node_ops;
        node.stream_ops = NODEFS.stream_ops;
        return node;
      },
  getMode(path) {
        var stat;
        try {
          stat = fs.lstatSync(path);
          if (NODEFS.isWindows) {
            // Node.js on Windows never represents permission bit 'x', so
            // propagate read bits to execute bits
            stat.mode = stat.mode | ((stat.mode & 292) >> 2);
          }
        } catch (e) {
          if (!e.code) throw e;
          throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
        }
        return stat.mode;
      },
  realPath(node) {
        var parts = [];
        while (node.parent !== node) {
          parts.push(node.name);
          node = node.parent;
        }
        parts.push(node.mount.opts.root);
        parts.reverse();
        return PATH.join.apply(null, parts);
      },
  flagsForNode(flags) {
        flags &= ~2097152; // Ignore this flag from musl, otherwise node.js fails to open the file.
        flags &= ~2048; // Ignore this flag from musl, otherwise node.js fails to open the file.
        flags &= ~32768; // Ignore this flag from musl, otherwise node.js fails to open the file.
        flags &= ~524288; // Some applications may pass it; it makes no sense for a single process.
        flags &= ~65536; // Node.js doesn't need this passed in, it errors.
        var newFlags = 0;
        for (var k in NODEFS.flagsForNodeMap) {
          if (flags & k) {
            newFlags |= NODEFS.flagsForNodeMap[k];
            flags ^= k;
          }
        }
        if (flags) {
          throw new FS.ErrnoError(28);
        }
        return newFlags;
      },
  node_ops:{
  getattr(node) {
          var path = NODEFS.realPath(node);
          var stat;
          try {
            stat = fs.lstatSync(path);
          } catch (e) {
            if (!e.code) throw e;
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
          // node.js v0.10.20 doesn't report blksize and blocks on Windows. Fake them with default blksize of 4096.
          // See http://support.microsoft.com/kb/140365
          if (NODEFS.isWindows && !stat.blksize) {
            stat.blksize = 4096;
          }
          if (NODEFS.isWindows && !stat.blocks) {
            stat.blocks = (stat.size+stat.blksize-1)/stat.blksize|0;
          }
          return {
            dev: stat.dev,
            ino: stat.ino,
            mode: stat.mode,
            nlink: stat.nlink,
            uid: stat.uid,
            gid: stat.gid,
            rdev: stat.rdev,
            size: stat.size,
            atime: stat.atime,
            mtime: stat.mtime,
            ctime: stat.ctime,
            blksize: stat.blksize,
            blocks: stat.blocks
          };
        },
  setattr(node, attr) {
          var path = NODEFS.realPath(node);
          try {
            if (attr.mode !== undefined) {
              fs.chmodSync(path, attr.mode);
              // update the common node structure mode as well
              node.mode = attr.mode;
            }
            if (attr.timestamp !== undefined) {
              var date = new Date(attr.timestamp);
              fs.utimesSync(path, date, date);
            }
            if (attr.size !== undefined) {
              fs.truncateSync(path, attr.size);
            }
          } catch (e) {
            if (!e.code) throw e;
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
        },
  lookup(parent, name) {
          var path = PATH.join2(NODEFS.realPath(parent), name);
          var mode = NODEFS.getMode(path);
          return NODEFS.createNode(parent, name, mode);
        },
  mknod(parent, name, mode, dev) {
          var node = NODEFS.createNode(parent, name, mode, dev);
          // create the backing node for this in the fs root as well
          var path = NODEFS.realPath(node);
          try {
            if (FS.isDir(node.mode)) {
              fs.mkdirSync(path, node.mode);
            } else {
              fs.writeFileSync(path, '', { mode: node.mode });
            }
          } catch (e) {
            if (!e.code) throw e;
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
          return node;
        },
  rename(oldNode, newDir, newName) {
          var oldPath = NODEFS.realPath(oldNode);
          var newPath = PATH.join2(NODEFS.realPath(newDir), newName);
          try {
            fs.renameSync(oldPath, newPath);
          } catch (e) {
            if (!e.code) throw e;
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
          oldNode.name = newName;
        },
  unlink(parent, name) {
          var path = PATH.join2(NODEFS.realPath(parent), name);
          try {
            fs.unlinkSync(path);
          } catch (e) {
            if (!e.code) throw e;
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
        },
  rmdir(parent, name) {
          var path = PATH.join2(NODEFS.realPath(parent), name);
          try {
            fs.rmdirSync(path);
          } catch (e) {
            if (!e.code) throw e;
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
        },
  readdir(node) {
          var path = NODEFS.realPath(node);
          try {
            return fs.readdirSync(path);
          } catch (e) {
            if (!e.code) throw e;
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
        },
  symlink(parent, newName, oldPath) {
          var newPath = PATH.join2(NODEFS.realPath(parent), newName);
          try {
            fs.symlinkSync(oldPath, newPath);
          } catch (e) {
            if (!e.code) throw e;
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
        },
  readlink(node) {
          var path = NODEFS.realPath(node);
          try {
            path = fs.readlinkSync(path);
            path = nodePath.relative(nodePath.resolve(node.mount.opts.root), path);
            return path;
          } catch (e) {
            if (!e.code) throw e;
            // node under windows can return code 'UNKNOWN' here:
            // https://github.com/emscripten-core/emscripten/issues/15468
            if (e.code === 'UNKNOWN') throw new FS.ErrnoError(28);
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
        },
  },
  stream_ops:{
  open(stream) {
          var path = NODEFS.realPath(stream.node);
          try {
            if (FS.isFile(stream.node.mode)) {
              stream.nfd = fs.openSync(path, NODEFS.flagsForNode(stream.flags));
            }
          } catch (e) {
            if (!e.code) throw e;
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
        },
  close(stream) {
          try {
            if (FS.isFile(stream.node.mode) && stream.nfd) {
              fs.closeSync(stream.nfd);
            }
          } catch (e) {
            if (!e.code) throw e;
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
        },
  read(stream, buffer, offset, length, position) {
          // Node.js < 6 compatibility: node errors on 0 length reads
          if (length === 0) return 0;
          try {
            return fs.readSync(stream.nfd, new Int8Array(buffer.buffer, offset, length), { position: position });
          } catch (e) {
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
        },
  write(stream, buffer, offset, length, position) {
          try {
            return fs.writeSync(stream.nfd, new Int8Array(buffer.buffer, offset, length), { position: position });
          } catch (e) {
            throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
          }
        },
  llseek(stream, offset, whence) {
          var position = offset;
          if (whence === 1) {
            position += stream.position;
          } else if (whence === 2) {
            if (FS.isFile(stream.node.mode)) {
              try {
                var stat = fs.fstatSync(stream.nfd);
                position += stat.size;
              } catch (e) {
                throw new FS.ErrnoError(NODEFS.convertNodeCode(e));
              }
            }
          }
  
          if (position < 0) {
            throw new FS.ErrnoError(28);
          }
  
          return position;
        },
  mmap(stream, length, position, prot, flags) {
          if (!FS.isFile(stream.node.mode)) {
            throw new FS.ErrnoError(43);
          }
  
          var ptr = mmapAlloc(length);
  
          NODEFS.stream_ops.read(stream, HEAP8, ptr, length, position);
          return { ptr, allocated: true };
        },
  msync(stream, buffer, offset, length, mmapFlags) {
          NODEFS.stream_ops.write(stream, buffer, 0, length, offset, false);
          // should we check if bytesWritten and length are the same?
          return 0;
        },
  },
  };
  
  var ERRNO_MESSAGES = {
  0:"Success",
  1:"Arg list too long",
  2:"Permission denied",
  3:"Address already in use",
  4:"Address not available",
  5:"Address family not supported by protocol family",
  6:"No more processes",
  7:"Socket already connected",
  8:"Bad file number",
  9:"Trying to read unreadable message",
  10:"Mount device busy",
  11:"Operation canceled",
  12:"No children",
  13:"Connection aborted",
  14:"Connection refused",
  15:"Connection reset by peer",
  16:"File locking deadlock error",
  17:"Destination address required",
  18:"Math arg out of domain of func",
  19:"Quota exceeded",
  20:"File exists",
  21:"Bad address",
  22:"File too large",
  23:"Host is unreachable",
  24:"Identifier removed",
  25:"Illegal byte sequence",
  26:"Connection already in progress",
  27:"Interrupted system call",
  28:"Invalid argument",
  29:"I/O error",
  30:"Socket is already connected",
  31:"Is a directory",
  32:"Too many symbolic links",
  33:"Too many open files",
  34:"Too many links",
  35:"Message too long",
  36:"Multihop attempted",
  37:"File or path name too long",
  38:"Network interface is not configured",
  39:"Connection reset by network",
  40:"Network is unreachable",
  41:"Too many open files in system",
  42:"No buffer space available",
  43:"No such device",
  44:"No such file or directory",
  45:"Exec format error",
  46:"No record locks available",
  47:"The link has been severed",
  48:"Not enough core",
  49:"No message of desired type",
  50:"Protocol not available",
  51:"No space left on device",
  52:"Function not implemented",
  53:"Socket is not connected",
  54:"Not a directory",
  55:"Directory not empty",
  56:"State not recoverable",
  57:"Socket operation on non-socket",
  59:"Not a typewriter",
  60:"No such device or address",
  61:"Value too large for defined data type",
  62:"Previous owner died",
  63:"Not super-user",
  64:"Broken pipe",
  65:"Protocol error",
  66:"Unknown protocol",
  67:"Protocol wrong type for socket",
  68:"Math result not representable",
  69:"Read only file system",
  70:"Illegal seek",
  71:"No such process",
  72:"Stale file handle",
  73:"Connection timed out",
  74:"Text file busy",
  75:"Cross-device link",
  100:"Device not a stream",
  101:"Bad font file fmt",
  102:"Invalid slot",
  103:"Invalid request code",
  104:"No anode",
  105:"Block device required",
  106:"Channel number out of range",
  107:"Level 3 halted",
  108:"Level 3 reset",
  109:"Link number out of range",
  110:"Protocol driver not attached",
  111:"No CSI structure available",
  112:"Level 2 halted",
  113:"Invalid exchange",
  114:"Invalid request descriptor",
  115:"Exchange full",
  116:"No data (for no delay io)",
  117:"Timer expired",
  118:"Out of streams resources",
  119:"Machine is not on the network",
  120:"Package not installed",
  121:"The object is remote",
  122:"Advertise error",
  123:"Srmount error",
  124:"Communication error on send",
  125:"Cross mount point (not really error)",
  126:"Given log. name not unique",
  127:"f.d. invalid for this operation",
  128:"Remote address changed",
  129:"Can   access a needed shared lib",
  130:"Accessing a corrupted shared lib",
  131:".lib section in a.out corrupted",
  132:"Attempting to link in too many libs",
  133:"Attempting to exec a shared library",
  135:"Streams pipe error",
  136:"Too many users",
  137:"Socket type not supported",
  138:"Not supported",
  139:"Protocol family not supported",
  140:"Can't send after socket shutdown",
  141:"Too many references",
  142:"Host is down",
  148:"No medium (in tape drive)",
  156:"Level 2 not synchronized",
  };
  
  
  var demangle = (func) => {
      warnOnce('warning: build with -sDEMANGLE_SUPPORT to link in libcxxabi demangling');
      return func;
    };
  var demangleAll = (text) => {
      var regex =
        /\b_Z[\w\d_]+/g;
      return text.replace(regex,
        function(x) {
          var y = demangle(x);
          return x === y ? x : (y + ' [' + x + ']');
        });
    };
  
  
  var LZ4 = {
  DIR_MODE:16895,
  FILE_MODE:33279,
  CHUNK_SIZE:-1,
  codec:null,
  init() {
        if (LZ4.codec) return;
        LZ4.codec = (function() {
          /*
  MiniLZ4: Minimal LZ4 block decoding and encoding.
  
  based off of node-lz4, https://github.com/pierrec/node-lz4
  
  ====
  Copyright (c) 2012 Pierre Curto
  
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
  
  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
  
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
  ====
  
  changes have the same license
  */
  
  var MiniLZ4 = (function() {
  
  var exports = {};
  
  /**
   * Decode a block. Assumptions: input contains all sequences of a 
   * chunk, output is large enough to receive the decoded data.
   * If the output buffer is too small, an error will be thrown.
   * If the returned value is negative, an error occured at the returned offset.
   *
   * @param {ArrayBufferView} input input data
   * @param {ArrayBufferView} output output data
   * @param {number=} sIdx
   * @param {number=} eIdx
   * @return {number} number of decoded bytes
   * @private
   */
  exports.uncompress = function (input, output, sIdx, eIdx) {
  	sIdx = sIdx || 0
  	eIdx = eIdx || (input.length - sIdx)
  	// Process each sequence in the incoming data
  	for (var i = sIdx, n = eIdx, j = 0; i < n;) {
  		var token = input[i++]
  
  		// Literals
  		var literals_length = (token >> 4)
  		if (literals_length > 0) {
  			// length of literals
  			var l = literals_length + 240
  			while (l === 255) {
  				l = input[i++]
  				literals_length += l
  			}
  
  			// Copy the literals
  			var end = i + literals_length
  			while (i < end) output[j++] = input[i++]
  
  			// End of buffer?
  			if (i === n) return j
  		}
  
  		// Match copy
  		// 2 bytes offset (little endian)
  		var offset = input[i++] | (input[i++] << 8)
  
  		// XXX 0 is an invalid offset value
  		if (offset === 0) return j
  		if (offset > j) return -(i-2)
  
  		// length of match copy
  		var match_length = (token & 0xf)
  		var l = match_length + 240
  		while (l === 255) {
  			l = input[i++]
  			match_length += l
  		}
  
  		// Copy the match
  		var pos = j - offset // position of the match copy in the current output
  		var end = j + match_length + 4 // minmatch = 4
  		while (j < end) output[j++] = output[pos++]
  	}
  
  	return j
  }
  
  var
  	maxInputSize	= 0x7E000000
  ,	minMatch		= 4
  // uint32() optimization
  ,	hashLog			= 16
  ,	hashShift		= (minMatch * 8) - hashLog
  ,	hashSize		= 1 << hashLog
  
  ,	copyLength		= 8
  ,	lastLiterals	= 5
  ,	mfLimit			= copyLength + minMatch
  ,	skipStrength	= 6
  
  ,	mlBits  		= 4
  ,	mlMask  		= (1 << mlBits) - 1
  ,	runBits 		= 8 - mlBits
  ,	runMask 		= (1 << runBits) - 1
  
  ,	hasher 			= /* XXX uint32( */ 2654435761 /* ) */
  
  assert(hashShift === 16);
  var hashTable = new Int16Array(1<<16);
  var empty = new Int16Array(hashTable.length);
  
  // CompressBound returns the maximum length of a lz4 block, given it's uncompressed length
  exports.compressBound = function (isize) {
  	return isize > maxInputSize
  		? 0
  		: (isize + (isize/255) + 16) | 0
  }
  
  /** @param {number=} sIdx
  	@param {number=} eIdx */
  exports.compress = function (src, dst, sIdx, eIdx) {
  	hashTable.set(empty);
  	return compressBlock(src, dst, 0, sIdx || 0, eIdx || dst.length)
  }
  
  function compressBlock (src, dst, pos, sIdx, eIdx) {
  	// XXX var Hash = uint32() // Reusable unsigned 32 bits integer
  	var dpos = sIdx
  	var dlen = eIdx - sIdx
  	var anchor = 0
  
  	if (src.length >= maxInputSize) throw new Error("input too large")
  
  	// Minimum of input bytes for compression (LZ4 specs)
  	if (src.length > mfLimit) {
  		var n = exports.compressBound(src.length)
  		if ( dlen < n ) throw Error("output too small: " + dlen + " < " + n)
  
  		var 
  			step  = 1
  		,	findMatchAttempts = (1 << skipStrength) + 3
  		// Keep last few bytes incompressible (LZ4 specs):
  		// last 5 bytes must be literals
  		,	srcLength = src.length - mfLimit
  
  		while (pos + minMatch < srcLength) {
  			// Find a match
  			// min match of 4 bytes aka sequence
  			var sequenceLowBits = src[pos+1]<<8 | src[pos]
  			var sequenceHighBits = src[pos+3]<<8 | src[pos+2]
  			// compute hash for the current sequence
  			var hash = Math.imul(sequenceLowBits | (sequenceHighBits << 16), hasher) >>> hashShift;
  			/* XXX Hash.fromBits(sequenceLowBits, sequenceHighBits)
  							.multiply(hasher)
  							.shiftr(hashShift)
  							.toNumber() */
  			// get the position of the sequence matching the hash
  			// NB. since 2 different sequences may have the same hash
  			// it is double-checked below
  			// do -1 to distinguish between initialized and uninitialized values
  			var ref = hashTable[hash] - 1
  			// save position of current sequence in hash table
  			hashTable[hash] = pos + 1
  
  			// first reference or within 64k limit or current sequence !== hashed one: no match
  			if ( ref < 0 ||
  				((pos - ref) >>> 16) > 0 ||
  				(
  					((src[ref+3]<<8 | src[ref+2]) != sequenceHighBits) ||
  					((src[ref+1]<<8 | src[ref]) != sequenceLowBits )
  				)
  			) {
  				// increase step if nothing found within limit
  				step = findMatchAttempts++ >> skipStrength
  				pos += step
  				continue
  			}
  
  			findMatchAttempts = (1 << skipStrength) + 3
  
  			// got a match
  			var literals_length = pos - anchor
  			var offset = pos - ref
  
  			// minMatch already verified
  			pos += minMatch
  			ref += minMatch
  
  			// move to the end of the match (>=minMatch)
  			var match_length = pos
  			while (pos < srcLength && src[pos] == src[ref]) {
  				pos++
  				ref++
  			}
  
  			// match length
  			match_length = pos - match_length
  
  			// token
  			var token = match_length < mlMask ? match_length : mlMask
  
  			// encode literals length
  			if (literals_length >= runMask) {
  				// add match length to the token
  				dst[dpos++] = (runMask << mlBits) + token
  				for (var len = literals_length - runMask; len > 254; len -= 255) {
  					dst[dpos++] = 255
  				}
  				dst[dpos++] = len
  			} else {
  				// add match length to the token
  				dst[dpos++] = (literals_length << mlBits) + token
  			}
  
  			// write literals
  			for (var i = 0; i < literals_length; i++) {
  				dst[dpos++] = src[anchor+i]
  			}
  
  			// encode offset
  			dst[dpos++] = offset
  			dst[dpos++] = (offset >> 8)
  
  			// encode match length
  			if (match_length >= mlMask) {
  				match_length -= mlMask
  				while (match_length >= 255) {
  					match_length -= 255
  					dst[dpos++] = 255
  				}
  
  				dst[dpos++] = match_length
  			}
  
  			anchor = pos
  		}
  	}
  
  	// cannot compress input
  	if (anchor == 0) return 0
  
  	// Write last literals
  	// encode literals length
  	literals_length = src.length - anchor
  	if (literals_length >= runMask) {
  		// add match length to the token
  		dst[dpos++] = (runMask << mlBits)
  		for (var ln = literals_length - runMask; ln > 254; ln -= 255) {
  			dst[dpos++] = 255
  		}
  		dst[dpos++] = ln
  	} else {
  		// add match length to the token
  		dst[dpos++] = (literals_length << mlBits)
  	}
  
  	// write literals
  	pos = anchor
  	while (pos < src.length) {
  		dst[dpos++] = src[pos++]
  	}
  
  	return dpos
  }
  
  exports.CHUNK_SIZE = 2048; // musl libc does readaheads of 1024 bytes, so a multiple of that is a good idea
  
  exports.compressPackage = function(data, verify) {
    if (verify) {
      var temp = new Uint8Array(exports.CHUNK_SIZE);
    }
    // compress the data in chunks
    assert(data instanceof ArrayBuffer);
    data = new Uint8Array(data);
    console.log('compressing package of size ' + data.length);
    var compressedChunks = [];
    var successes = [];
    var offset = 0;
    var total = 0;
    while (offset < data.length) {
      var chunk = data.subarray(offset, offset + exports.CHUNK_SIZE);
      //console.log('compress a chunk ' + [offset, total, data.length]);
      offset += exports.CHUNK_SIZE;
      var bound = exports.compressBound(chunk.length);
      var compressed = new Uint8Array(bound);
      var compressedSize = exports.compress(chunk, compressed);
      if (compressedSize > 0) {
        assert(compressedSize <= bound);
        compressed = compressed.subarray(0, compressedSize);
        compressedChunks.push(compressed);
        total += compressedSize;
        successes.push(1);
        if (verify) {
          var back = exports.uncompress(compressed, temp);
          assert(back === chunk.length, [back, chunk.length]);
          for (var i = 0; i < chunk.length; i++) {
            assert(chunk[i] === temp[i]);
          }
        }
      } else {
        assert(compressedSize === 0);
        // failure to compress :(
        compressedChunks.push(chunk);
        total += chunk.length; // last chunk may not be the full exports.CHUNK_SIZE size
        successes.push(0);
      }
    }
    data = null; // XXX null out pack['data'] too?
    var compressedData = {
      'data': new Uint8Array(total + exports.CHUNK_SIZE*2), // store all the compressed data, plus room for two cached decompressed chunk, in one fast array
      'cachedOffset': total,
      'cachedIndexes': [-1, -1], // cache last two blocks, so that reading 1,2,3 + preloading another block won't trigger decompress thrashing
      'cachedChunks': [null, null],
      'offsets': [], // chunk# => start in compressed data
      'sizes': [],
      'successes': successes, // 1 if chunk is compressed
    };
    offset = 0;
    for (var i = 0; i < compressedChunks.length; i++) {
      compressedData['data'].set(compressedChunks[i], offset);
      compressedData['offsets'][i] = offset;
      compressedData['sizes'][i] = compressedChunks[i].length
      offset += compressedChunks[i].length;
    }
    console.log('compressed package into ' + [compressedData['data'].length]);
    assert(offset === total);
    return compressedData;
  };
  
  assert(exports.CHUNK_SIZE < (1 << 15)); // we use 16-bit ints as the type of the hash table, chunk size must be smaller
  
  return exports;
  
  })();
  
  ;
          return MiniLZ4;
        })();
        LZ4.CHUNK_SIZE = LZ4.codec.CHUNK_SIZE;
      },
  loadPackage(pack, preloadPlugin) {
        LZ4.init();
        var compressedData = pack['compressedData'];
        if (!compressedData) compressedData = LZ4.codec.compressPackage(pack['data']);
        assert(compressedData['cachedIndexes'].length === compressedData['cachedChunks'].length);
        for (var i = 0; i < compressedData['cachedIndexes'].length; i++) {
          compressedData['cachedIndexes'][i] = -1;
          compressedData['cachedChunks'][i] = compressedData['data'].subarray(compressedData['cachedOffset'] + i*LZ4.CHUNK_SIZE,
                                                                        compressedData['cachedOffset'] + (i+1)*LZ4.CHUNK_SIZE);
          assert(compressedData['cachedChunks'][i].length === LZ4.CHUNK_SIZE);
        }
        pack['metadata'].files.forEach((file) => {
          var dir = PATH.dirname(file.filename);
          var name = PATH.basename(file.filename);
          FS.createPath('', dir, true, true);
          var parent = FS.analyzePath(dir).object;
          LZ4.createNode(parent, name, LZ4.FILE_MODE, 0, {
            compressedData,
            start: file.start,
            end: file.end,
          });
        });
        // Preload files if necessary. This code is largely similar to
        // createPreloadedFile in library_fs.js. However, a main difference here
        // is that we only decompress the file if it can be preloaded.
        // Abstracting out the common parts seems to be more effort than it is
        // worth.
        if (preloadPlugin) {
          Browser.init();
          pack['metadata'].files.forEach((file) => {
            var handled = false;
            var fullname = file.filename;
            preloadPlugins.forEach((plugin) => {
              if (handled) return;
              if (plugin['canHandle'](fullname)) {
                var dep = getUniqueRunDependency('fp ' + fullname);
                addRunDependency(dep);
                var finish = () => removeRunDependency(dep);
                var byteArray = FS.readFile(fullname);
                plugin['handle'](byteArray, fullname, finish, finish);
                handled = true;
              }
            });
          });
        }
      },
  createNode(parent, name, mode, dev, contents, mtime) {
        var node = FS.createNode(parent, name, mode);
        node.mode = mode;
        node.node_ops = LZ4.node_ops;
        node.stream_ops = LZ4.stream_ops;
        node.timestamp = (mtime || new Date).getTime();
        assert(LZ4.FILE_MODE !== LZ4.DIR_MODE);
        if (mode === LZ4.FILE_MODE) {
          node.size = contents.end - contents.start;
          node.contents = contents;
        } else {
          node.size = 4096;
          node.contents = {};
        }
        if (parent) {
          parent.contents[name] = node;
        }
        return node;
      },
  node_ops:{
  getattr(node) {
          return {
            dev: 1,
            ino: node.id,
            mode: node.mode,
            nlink: 1,
            uid: 0,
            gid: 0,
            rdev: 0,
            size: node.size,
            atime: new Date(node.timestamp),
            mtime: new Date(node.timestamp),
            ctime: new Date(node.timestamp),
            blksize: 4096,
            blocks: Math.ceil(node.size / 4096),
          };
        },
  setattr(node, attr) {
          if (attr.mode !== undefined) {
            node.mode = attr.mode;
          }
          if (attr.timestamp !== undefined) {
            node.timestamp = attr.timestamp;
          }
        },
  lookup(parent, name) {
          throw new FS.ErrnoError(44);
        },
  mknod(parent, name, mode, dev) {
          throw new FS.ErrnoError(63);
        },
  rename(oldNode, newDir, newName) {
          throw new FS.ErrnoError(63);
        },
  unlink(parent, name) {
          throw new FS.ErrnoError(63);
        },
  rmdir(parent, name) {
          throw new FS.ErrnoError(63);
        },
  readdir(node) {
          throw new FS.ErrnoError(63);
        },
  symlink(parent, newName, oldPath) {
          throw new FS.ErrnoError(63);
        },
  },
  stream_ops:{
  read(stream, buffer, offset, length, position) {
          //out('LZ4 read ' + [offset, length, position]);
          length = Math.min(length, stream.node.size - position);
          if (length <= 0) return 0;
          var contents = stream.node.contents;
          var compressedData = contents.compressedData;
          var written = 0;
          while (written < length) {
            var start = contents.start + position + written; // start index in uncompressed data
            var desired = length - written;
            //out('current read: ' + ['start', start, 'desired', desired]);
            var chunkIndex = Math.floor(start / LZ4.CHUNK_SIZE);
            var compressedStart = compressedData['offsets'][chunkIndex];
            var compressedSize = compressedData['sizes'][chunkIndex];
            var currChunk;
            if (compressedData['successes'][chunkIndex]) {
              var found = compressedData['cachedIndexes'].indexOf(chunkIndex);
              if (found >= 0) {
                currChunk = compressedData['cachedChunks'][found];
              } else {
                // decompress the chunk
                compressedData['cachedIndexes'].pop();
                compressedData['cachedIndexes'].unshift(chunkIndex);
                currChunk = compressedData['cachedChunks'].pop();
                compressedData['cachedChunks'].unshift(currChunk);
                if (compressedData['debug']) {
                  out('decompressing chunk ' + chunkIndex);
                  Module['decompressedChunks'] = (Module['decompressedChunks'] || 0) + 1;
                }
                var compressed = compressedData['data'].subarray(compressedStart, compressedStart + compressedSize);
                //var t = Date.now();
                var originalSize = LZ4.codec.uncompress(compressed, currChunk);
                //out('decompress time: ' + (Date.now() - t));
                if (chunkIndex < compressedData['successes'].length-1) assert(originalSize === LZ4.CHUNK_SIZE); // all but the last chunk must be full-size
              }
            } else {
              // uncompressed
              currChunk = compressedData['data'].subarray(compressedStart, compressedStart + LZ4.CHUNK_SIZE);
            }
            var startInChunk = start % LZ4.CHUNK_SIZE;
            var endInChunk = Math.min(startInChunk + desired, LZ4.CHUNK_SIZE);
            buffer.set(currChunk.subarray(startInChunk, endInChunk), offset + written);
            var currWritten = endInChunk - startInChunk;
            written += currWritten;
          }
          return written;
        },
  write(stream, buffer, offset, length, position) {
          throw new FS.ErrnoError(29);
        },
  llseek(stream, offset, whence) {
          var position = offset;
          if (whence === 1) {
            position += stream.position;
          } else if (whence === 2) {
            if (FS.isFile(stream.node.mode)) {
              position += stream.node.size;
            }
          }
          if (position < 0) {
            throw new FS.ErrnoError(28);
          }
          return position;
        },
  },
  };
  var FS = {
  root:null,
  mounts:[],
  devices:{
  },
  streams:[],
  nextInode:1,
  nameTable:null,
  currentPath:"/",
  initialized:false,
  ignorePermissions:true,
  ErrnoError:null,
  genericErrors:{
  },
  filesystems:null,
  syncFSRequests:0,
  lookupPath(path, opts = {}) {
        path = PATH_FS.resolve(path);
  
        if (!path) return { path: '', node: null };
  
        var defaults = {
          follow_mount: true,
          recurse_count: 0
        };
        opts = Object.assign(defaults, opts)
  
        if (opts.recurse_count > 8) {  // max recursive lookup of 8
          throw new FS.ErrnoError(32);
        }
  
        // split the absolute path
        var parts = path.split('/').filter((p) => !!p);
  
        // start at the root
        var current = FS.root;
        var current_path = '/';
  
        for (var i = 0; i < parts.length; i++) {
          var islast = (i === parts.length-1);
          if (islast && opts.parent) {
            // stop resolving
            break;
          }
  
          current = FS.lookupNode(current, parts[i]);
          current_path = PATH.join2(current_path, parts[i]);
  
          // jump to the mount's root node if this is a mountpoint
          if (FS.isMountpoint(current)) {
            if (!islast || (islast && opts.follow_mount)) {
              current = current.mounted.root;
            }
          }
  
          // by default, lookupPath will not follow a symlink if it is the final path component.
          // setting opts.follow = true will override this behavior.
          if (!islast || opts.follow) {
            var count = 0;
            while (FS.isLink(current.mode)) {
              var link = FS.readlink(current_path);
              current_path = PATH_FS.resolve(PATH.dirname(current_path), link);
  
              var lookup = FS.lookupPath(current_path, { recurse_count: opts.recurse_count + 1 });
              current = lookup.node;
  
              if (count++ > 40) {  // limit max consecutive symlinks to 40 (SYMLOOP_MAX).
                throw new FS.ErrnoError(32);
              }
            }
          }
        }
  
        return { path: current_path, node: current };
      },
  getPath(node) {
        var path;
        while (true) {
          if (FS.isRoot(node)) {
            var mount = node.mount.mountpoint;
            if (!path) return mount;
            return mount[mount.length-1] !== '/' ? `${mount}/${path}` : mount + path;
          }
          path = path ? `${node.name}/${path}` : node.name;
          node = node.parent;
        }
      },
  hashName(parentid, name) {
        var hash = 0;
  
        for (var i = 0; i < name.length; i++) {
          hash = ((hash << 5) - hash + name.charCodeAt(i)) | 0;
        }
        return ((parentid + hash) >>> 0) % FS.nameTable.length;
      },
  hashAddNode(node) {
        var hash = FS.hashName(node.parent.id, node.name);
        node.name_next = FS.nameTable[hash];
        FS.nameTable[hash] = node;
      },
  hashRemoveNode(node) {
        var hash = FS.hashName(node.parent.id, node.name);
        if (FS.nameTable[hash] === node) {
          FS.nameTable[hash] = node.name_next;
        } else {
          var current = FS.nameTable[hash];
          while (current) {
            if (current.name_next === node) {
              current.name_next = node.name_next;
              break;
            }
            current = current.name_next;
          }
        }
      },
  lookupNode(parent, name) {
        var errCode = FS.mayLookup(parent);
        if (errCode) {
          throw new FS.ErrnoError(errCode, parent);
        }
        var hash = FS.hashName(parent.id, name);
        for (var node = FS.nameTable[hash]; node; node = node.name_next) {
          var nodeName = node.name;
          if (node.parent.id === parent.id && nodeName === name) {
            return node;
          }
        }
        // if we failed to find it in the cache, call into the VFS
        return FS.lookup(parent, name);
      },
  createNode(parent, name, mode, rdev) {
        assert(typeof parent == 'object')
        var node = new FS.FSNode(parent, name, mode, rdev);
  
        FS.hashAddNode(node);
  
        return node;
      },
  destroyNode(node) {
        FS.hashRemoveNode(node);
      },
  isRoot(node) {
        return node === node.parent;
      },
  isMountpoint(node) {
        return !!node.mounted;
      },
  isFile(mode) {
        return (mode & 61440) === 32768;
      },
  isDir(mode) {
        return (mode & 61440) === 16384;
      },
  isLink(mode) {
        return (mode & 61440) === 40960;
      },
  isChrdev(mode) {
        return (mode & 61440) === 8192;
      },
  isBlkdev(mode) {
        return (mode & 61440) === 24576;
      },
  isFIFO(mode) {
        return (mode & 61440) === 4096;
      },
  isSocket(mode) {
        return (mode & 49152) === 49152;
      },
  flagsToPermissionString(flag) {
        var perms = ['r', 'w', 'rw'][flag & 3];
        if ((flag & 512)) {
          perms += 'w';
        }
        return perms;
      },
  nodePermissions(node, perms) {
        if (FS.ignorePermissions) {
          return 0;
        }
        // return 0 if any user, group or owner bits are set.
        if (perms.includes('r') && !(node.mode & 292)) {
          return 2;
        } else if (perms.includes('w') && !(node.mode & 146)) {
          return 2;
        } else if (perms.includes('x') && !(node.mode & 73)) {
          return 2;
        }
        return 0;
      },
  mayLookup(dir) {
        var errCode = FS.nodePermissions(dir, 'x');
        if (errCode) return errCode;
        if (!dir.node_ops.lookup) return 2;
        return 0;
      },
  mayCreate(dir, name) {
        try {
          var node = FS.lookupNode(dir, name);
          return 20;
        } catch (e) {
        }
        return FS.nodePermissions(dir, 'wx');
      },
  mayDelete(dir, name, isdir) {
        var node;
        try {
          node = FS.lookupNode(dir, name);
        } catch (e) {
          return e.errno;
        }
        var errCode = FS.nodePermissions(dir, 'wx');
        if (errCode) {
          return errCode;
        }
        if (isdir) {
          if (!FS.isDir(node.mode)) {
            return 54;
          }
          if (FS.isRoot(node) || FS.getPath(node) === FS.cwd()) {
            return 10;
          }
        } else {
          if (FS.isDir(node.mode)) {
            return 31;
          }
        }
        return 0;
      },
  mayOpen(node, flags) {
        if (!node) {
          return 44;
        }
        if (FS.isLink(node.mode)) {
          return 32;
        } else if (FS.isDir(node.mode)) {
          if (FS.flagsToPermissionString(flags) !== 'r' || // opening for write
              (flags & 512)) { // TODO: check for O_SEARCH? (== search for dir only)
            return 31;
          }
        }
        return FS.nodePermissions(node, FS.flagsToPermissionString(flags));
      },
  MAX_OPEN_FDS:4096,
  nextfd() {
        for (var fd = 0; fd <= FS.MAX_OPEN_FDS; fd++) {
          if (!FS.streams[fd]) {
            return fd;
          }
        }
        throw new FS.ErrnoError(33);
      },
  getStreamChecked(fd) {
        var stream = FS.getStream(fd);
        if (!stream) {
          throw new FS.ErrnoError(8);
        }
        return stream;
      },
  getStream:(fd) => FS.streams[fd],
  createStream(stream, fd = -1) {
        if (!FS.FSStream) {
          FS.FSStream = /** @constructor */ function() {
            this.shared = { };
          };
          FS.FSStream.prototype = {};
          Object.defineProperties(FS.FSStream.prototype, {
            object: {
              /** @this {FS.FSStream} */
              get() { return this.node; },
              /** @this {FS.FSStream} */
              set(val) { this.node = val; }
            },
            isRead: {
              /** @this {FS.FSStream} */
              get() { return (this.flags & 2097155) !== 1; }
            },
            isWrite: {
              /** @this {FS.FSStream} */
              get() { return (this.flags & 2097155) !== 0; }
            },
            isAppend: {
              /** @this {FS.FSStream} */
              get() { return (this.flags & 1024); }
            },
            flags: {
              /** @this {FS.FSStream} */
              get() { return this.shared.flags; },
              /** @this {FS.FSStream} */
              set(val) { this.shared.flags = val; },
            },
            position : {
              /** @this {FS.FSStream} */
              get() { return this.shared.position; },
              /** @this {FS.FSStream} */
              set(val) { this.shared.position = val; },
            },
          });
        }
        // clone it, so we can return an instance of FSStream
        stream = Object.assign(new FS.FSStream(), stream);
        if (fd == -1) {
          fd = FS.nextfd();
        }
        stream.fd = fd;
        FS.streams[fd] = stream;
        return stream;
      },
  closeStream(fd) {
        FS.streams[fd] = null;
      },
  chrdev_stream_ops:{
  open(stream) {
          var device = FS.getDevice(stream.node.rdev);
          // override node's stream ops with the device's
          stream.stream_ops = device.stream_ops;
          // forward the open call
          if (stream.stream_ops.open) {
            stream.stream_ops.open(stream);
          }
        },
  llseek() {
          throw new FS.ErrnoError(70);
        },
  },
  major:(dev) => ((dev) >> 8),
  minor:(dev) => ((dev) & 0xff),
  makedev:(ma, mi) => ((ma) << 8 | (mi)),
  registerDevice(dev, ops) {
        FS.devices[dev] = { stream_ops: ops };
      },
  getDevice:(dev) => FS.devices[dev],
  getMounts(mount) {
        var mounts = [];
        var check = [mount];
  
        while (check.length) {
          var m = check.pop();
  
          mounts.push(m);
  
          check.push.apply(check, m.mounts);
        }
  
        return mounts;
      },
  syncfs(populate, callback) {
        if (typeof populate == 'function') {
          callback = populate;
          populate = false;
        }
  
        FS.syncFSRequests++;
  
        if (FS.syncFSRequests > 1) {
          err(`warning: ${FS.syncFSRequests} FS.syncfs operations in flight at once, probably just doing extra work`);
        }
  
        var mounts = FS.getMounts(FS.root.mount);
        var completed = 0;
  
        function doCallback(errCode) {
          assert(FS.syncFSRequests > 0);
          FS.syncFSRequests--;
          return callback(errCode);
        }
  
        function done(errCode) {
          if (errCode) {
            if (!done.errored) {
              done.errored = true;
              return doCallback(errCode);
            }
            return;
          }
          if (++completed >= mounts.length) {
            doCallback(null);
          }
        };
  
        // sync all mounts
        mounts.forEach((mount) => {
          if (!mount.type.syncfs) {
            return done(null);
          }
          mount.type.syncfs(mount, populate, done);
        });
      },
  mount(type, opts, mountpoint) {
        if (typeof type == 'string') {
          // The filesystem was not included, and instead we have an error
          // message stored in the variable.
          throw type;
        }
        var root = mountpoint === '/';
        var pseudo = !mountpoint;
        var node;
  
        if (root && FS.root) {
          throw new FS.ErrnoError(10);
        } else if (!root && !pseudo) {
          var lookup = FS.lookupPath(mountpoint, { follow_mount: false });
  
          mountpoint = lookup.path;  // use the absolute path
          node = lookup.node;
  
          if (FS.isMountpoint(node)) {
            throw new FS.ErrnoError(10);
          }
  
          if (!FS.isDir(node.mode)) {
            throw new FS.ErrnoError(54);
          }
        }
  
        var mount = {
          type,
          opts,
          mountpoint,
          mounts: []
        };
  
        // create a root node for the fs
        var mountRoot = type.mount(mount);
        mountRoot.mount = mount;
        mount.root = mountRoot;
  
        if (root) {
          FS.root = mountRoot;
        } else if (node) {
          // set as a mountpoint
          node.mounted = mount;
  
          // add the new mount to the current mount's children
          if (node.mount) {
            node.mount.mounts.push(mount);
          }
        }
  
        return mountRoot;
      },
  unmount(mountpoint) {
        var lookup = FS.lookupPath(mountpoint, { follow_mount: false });
  
        if (!FS.isMountpoint(lookup.node)) {
          throw new FS.ErrnoError(28);
        }
  
        // destroy the nodes for this mount, and all its child mounts
        var node = lookup.node;
        var mount = node.mounted;
        var mounts = FS.getMounts(mount);
  
        Object.keys(FS.nameTable).forEach((hash) => {
          var current = FS.nameTable[hash];
  
          while (current) {
            var next = current.name_next;
  
            if (mounts.includes(current.mount)) {
              FS.destroyNode(current);
            }
  
            current = next;
          }
        });
  
        // no longer a mountpoint
        node.mounted = null;
  
        // remove this mount from the child mounts
        var idx = node.mount.mounts.indexOf(mount);
        assert(idx !== -1);
        node.mount.mounts.splice(idx, 1);
      },
  lookup(parent, name) {
        return parent.node_ops.lookup(parent, name);
      },
  mknod(path, mode, dev) {
        var lookup = FS.lookupPath(path, { parent: true });
        var parent = lookup.node;
        var name = PATH.basename(path);
        if (!name || name === '.' || name === '..') {
          throw new FS.ErrnoError(28);
        }
        var errCode = FS.mayCreate(parent, name);
        if (errCode) {
          throw new FS.ErrnoError(errCode);
        }
        if (!parent.node_ops.mknod) {
          throw new FS.ErrnoError(63);
        }
        return parent.node_ops.mknod(parent, name, mode, dev);
      },
  create(path, mode) {
        mode = mode !== undefined ? mode : 438 /* 0666 */;
        mode &= 4095;
        mode |= 32768;
        return FS.mknod(path, mode, 0);
      },
  mkdir(path, mode) {
        mode = mode !== undefined ? mode : 511 /* 0777 */;
        mode &= 511 | 512;
        mode |= 16384;
        return FS.mknod(path, mode, 0);
      },
  mkdirTree(path, mode) {
        var dirs = path.split('/');
        var d = '';
        for (var i = 0; i < dirs.length; ++i) {
          if (!dirs[i]) continue;
          d += '/' + dirs[i];
          try {
            FS.mkdir(d, mode);
          } catch(e) {
            if (e.errno != 20) throw e;
          }
        }
      },
  mkdev(path, mode, dev) {
        if (typeof dev == 'undefined') {
          dev = mode;
          mode = 438 /* 0666 */;
        }
        mode |= 8192;
        return FS.mknod(path, mode, dev);
      },
  symlink(oldpath, newpath) {
        if (!PATH_FS.resolve(oldpath)) {
          throw new FS.ErrnoError(44);
        }
        var lookup = FS.lookupPath(newpath, { parent: true });
        var parent = lookup.node;
        if (!parent) {
          throw new FS.ErrnoError(44);
        }
        var newname = PATH.basename(newpath);
        var errCode = FS.mayCreate(parent, newname);
        if (errCode) {
          throw new FS.ErrnoError(errCode);
        }
        if (!parent.node_ops.symlink) {
          throw new FS.ErrnoError(63);
        }
        return parent.node_ops.symlink(parent, newname, oldpath);
      },
  rename(old_path, new_path) {
        var old_dirname = PATH.dirname(old_path);
        var new_dirname = PATH.dirname(new_path);
        var old_name = PATH.basename(old_path);
        var new_name = PATH.basename(new_path);
        // parents must exist
        var lookup, old_dir, new_dir;
  
        // let the errors from non existant directories percolate up
        lookup = FS.lookupPath(old_path, { parent: true });
        old_dir = lookup.node;
        lookup = FS.lookupPath(new_path, { parent: true });
        new_dir = lookup.node;
  
        if (!old_dir || !new_dir) throw new FS.ErrnoError(44);
        // need to be part of the same mount
        if (old_dir.mount !== new_dir.mount) {
          throw new FS.ErrnoError(75);
        }
        // source must exist
        var old_node = FS.lookupNode(old_dir, old_name);
        // old path should not be an ancestor of the new path
        var relative = PATH_FS.relative(old_path, new_dirname);
        if (relative.charAt(0) !== '.') {
          throw new FS.ErrnoError(28);
        }
        // new path should not be an ancestor of the old path
        relative = PATH_FS.relative(new_path, old_dirname);
        if (relative.charAt(0) !== '.') {
          throw new FS.ErrnoError(55);
        }
        // see if the new path already exists
        var new_node;
        try {
          new_node = FS.lookupNode(new_dir, new_name);
        } catch (e) {
          // not fatal
        }
        // early out if nothing needs to change
        if (old_node === new_node) {
          return;
        }
        // we'll need to delete the old entry
        var isdir = FS.isDir(old_node.mode);
        var errCode = FS.mayDelete(old_dir, old_name, isdir);
        if (errCode) {
          throw new FS.ErrnoError(errCode);
        }
        // need delete permissions if we'll be overwriting.
        // need create permissions if new doesn't already exist.
        errCode = new_node ?
          FS.mayDelete(new_dir, new_name, isdir) :
          FS.mayCreate(new_dir, new_name);
        if (errCode) {
          throw new FS.ErrnoError(errCode);
        }
        if (!old_dir.node_ops.rename) {
          throw new FS.ErrnoError(63);
        }
        if (FS.isMountpoint(old_node) || (new_node && FS.isMountpoint(new_node))) {
          throw new FS.ErrnoError(10);
        }
        // if we are going to change the parent, check write permissions
        if (new_dir !== old_dir) {
          errCode = FS.nodePermissions(old_dir, 'w');
          if (errCode) {
            throw new FS.ErrnoError(errCode);
          }
        }
        // remove the node from the lookup hash
        FS.hashRemoveNode(old_node);
        // do the underlying fs rename
        try {
          old_dir.node_ops.rename(old_node, new_dir, new_name);
        } catch (e) {
          throw e;
        } finally {
          // add the node back to the hash (in case node_ops.rename
          // changed its name)
          FS.hashAddNode(old_node);
        }
      },
  rmdir(path) {
        var lookup = FS.lookupPath(path, { parent: true });
        var parent = lookup.node;
        var name = PATH.basename(path);
        var node = FS.lookupNode(parent, name);
        var errCode = FS.mayDelete(parent, name, true);
        if (errCode) {
          throw new FS.ErrnoError(errCode);
        }
        if (!parent.node_ops.rmdir) {
          throw new FS.ErrnoError(63);
        }
        if (FS.isMountpoint(node)) {
          throw new FS.ErrnoError(10);
        }
        parent.node_ops.rmdir(parent, name);
        FS.destroyNode(node);
      },
  readdir(path) {
        var lookup = FS.lookupPath(path, { follow: true });
        var node = lookup.node;
        if (!node.node_ops.readdir) {
          throw new FS.ErrnoError(54);
        }
        return node.node_ops.readdir(node);
      },
  unlink(path) {
        var lookup = FS.lookupPath(path, { parent: true });
        var parent = lookup.node;
        if (!parent) {
          throw new FS.ErrnoError(44);
        }
        var name = PATH.basename(path);
        var node = FS.lookupNode(parent, name);
        var errCode = FS.mayDelete(parent, name, false);
        if (errCode) {
          // According to POSIX, we should map EISDIR to EPERM, but
          // we instead do what Linux does (and we must, as we use
          // the musl linux libc).
          throw new FS.ErrnoError(errCode);
        }
        if (!parent.node_ops.unlink) {
          throw new FS.ErrnoError(63);
        }
        if (FS.isMountpoint(node)) {
          throw new FS.ErrnoError(10);
        }
        parent.node_ops.unlink(parent, name);
        FS.destroyNode(node);
      },
  readlink(path) {
        var lookup = FS.lookupPath(path);
        var link = lookup.node;
        if (!link) {
          throw new FS.ErrnoError(44);
        }
        if (!link.node_ops.readlink) {
          throw new FS.ErrnoError(28);
        }
        return PATH_FS.resolve(FS.getPath(link.parent), link.node_ops.readlink(link));
      },
  stat(path, dontFollow) {
        var lookup = FS.lookupPath(path, { follow: !dontFollow });
        var node = lookup.node;
        if (!node) {
          throw new FS.ErrnoError(44);
        }
        if (!node.node_ops.getattr) {
          throw new FS.ErrnoError(63);
        }
        return node.node_ops.getattr(node);
      },
  lstat(path) {
        return FS.stat(path, true);
      },
  chmod(path, mode, dontFollow) {
        var node;
        if (typeof path == 'string') {
          var lookup = FS.lookupPath(path, { follow: !dontFollow });
          node = lookup.node;
        } else {
          node = path;
        }
        if (!node.node_ops.setattr) {
          throw new FS.ErrnoError(63);
        }
        node.node_ops.setattr(node, {
          mode: (mode & 4095) | (node.mode & ~4095),
          timestamp: Date.now()
        });
      },
  lchmod(path, mode) {
        FS.chmod(path, mode, true);
      },
  fchmod(fd, mode) {
        var stream = FS.getStreamChecked(fd);
        FS.chmod(stream.node, mode);
      },
  chown(path, uid, gid, dontFollow) {
        var node;
        if (typeof path == 'string') {
          var lookup = FS.lookupPath(path, { follow: !dontFollow });
          node = lookup.node;
        } else {
          node = path;
        }
        if (!node.node_ops.setattr) {
          throw new FS.ErrnoError(63);
        }
        node.node_ops.setattr(node, {
          timestamp: Date.now()
          // we ignore the uid / gid for now
        });
      },
  lchown(path, uid, gid) {
        FS.chown(path, uid, gid, true);
      },
  fchown(fd, uid, gid) {
        var stream = FS.getStreamChecked(fd);
        FS.chown(stream.node, uid, gid);
      },
  truncate(path, len) {
        if (len < 0) {
          throw new FS.ErrnoError(28);
        }
        var node;
        if (typeof path == 'string') {
          var lookup = FS.lookupPath(path, { follow: true });
          node = lookup.node;
        } else {
          node = path;
        }
        if (!node.node_ops.setattr) {
          throw new FS.ErrnoError(63);
        }
        if (FS.isDir(node.mode)) {
          throw new FS.ErrnoError(31);
        }
        if (!FS.isFile(node.mode)) {
          throw new FS.ErrnoError(28);
        }
        var errCode = FS.nodePermissions(node, 'w');
        if (errCode) {
          throw new FS.ErrnoError(errCode);
        }
        node.node_ops.setattr(node, {
          size: len,
          timestamp: Date.now()
        });
      },
  ftruncate(fd, len) {
        var stream = FS.getStreamChecked(fd);
        if ((stream.flags & 2097155) === 0) {
          throw new FS.ErrnoError(28);
        }
        FS.truncate(stream.node, len);
      },
  utime(path, atime, mtime) {
        var lookup = FS.lookupPath(path, { follow: true });
        var node = lookup.node;
        node.node_ops.setattr(node, {
          timestamp: Math.max(atime, mtime)
        });
      },
  open(path, flags, mode) {
        if (path === "") {
          throw new FS.ErrnoError(44);
        }
        flags = typeof flags == 'string' ? FS_modeStringToFlags(flags) : flags;
        mode = typeof mode == 'undefined' ? 438 /* 0666 */ : mode;
        if ((flags & 64)) {
          mode = (mode & 4095) | 32768;
        } else {
          mode = 0;
        }
        var node;
        if (typeof path == 'object') {
          node = path;
        } else {
          path = PATH.normalize(path);
          try {
            var lookup = FS.lookupPath(path, {
              follow: !(flags & 131072)
            });
            node = lookup.node;
          } catch (e) {
            // ignore
          }
        }
        // perhaps we need to create the node
        var created = false;
        if ((flags & 64)) {
          if (node) {
            // if O_CREAT and O_EXCL are set, error out if the node already exists
            if ((flags & 128)) {
              throw new FS.ErrnoError(20);
            }
          } else {
            // node doesn't exist, try to create it
            node = FS.mknod(path, mode, 0);
            created = true;
          }
        }
        if (!node) {
          throw new FS.ErrnoError(44);
        }
        // can't truncate a device
        if (FS.isChrdev(node.mode)) {
          flags &= ~512;
        }
        // if asked only for a directory, then this must be one
        if ((flags & 65536) && !FS.isDir(node.mode)) {
          throw new FS.ErrnoError(54);
        }
        // check permissions, if this is not a file we just created now (it is ok to
        // create and write to a file with read-only permissions; it is read-only
        // for later use)
        if (!created) {
          var errCode = FS.mayOpen(node, flags);
          if (errCode) {
            throw new FS.ErrnoError(errCode);
          }
        }
        // do truncation if necessary
        if ((flags & 512) && !created) {
          FS.truncate(node, 0);
        }
        // we've already handled these, don't pass down to the underlying vfs
        flags &= ~(128 | 512 | 131072);
  
        // register the stream with the filesystem
        var stream = FS.createStream({
          node,
          path: FS.getPath(node),  // we want the absolute path to the node
          flags,
          seekable: true,
          position: 0,
          stream_ops: node.stream_ops,
          // used by the file family libc calls (fopen, fwrite, ferror, etc.)
          ungotten: [],
          error: false
        });
        // call the new stream's open function
        if (stream.stream_ops.open) {
          stream.stream_ops.open(stream);
        }
        if (Module['logReadFiles'] && !(flags & 1)) {
          if (!FS.readFiles) FS.readFiles = {};
          if (!(path in FS.readFiles)) {
            FS.readFiles[path] = 1;
          }
        }
        return stream;
      },
  close(stream) {
        if (FS.isClosed(stream)) {
          throw new FS.ErrnoError(8);
        }
        if (stream.getdents) stream.getdents = null; // free readdir state
        try {
          if (stream.stream_ops.close) {
            stream.stream_ops.close(stream);
          }
        } catch (e) {
          throw e;
        } finally {
          FS.closeStream(stream.fd);
        }
        stream.fd = null;
      },
  isClosed(stream) {
        return stream.fd === null;
      },
  llseek(stream, offset, whence) {
        if (FS.isClosed(stream)) {
          throw new FS.ErrnoError(8);
        }
        if (!stream.seekable || !stream.stream_ops.llseek) {
          throw new FS.ErrnoError(70);
        }
        if (whence != 0 && whence != 1 && whence != 2) {
          throw new FS.ErrnoError(28);
        }
        stream.position = stream.stream_ops.llseek(stream, offset, whence);
        stream.ungotten = [];
        return stream.position;
      },
  read(stream, buffer, offset, length, position) {
        assert(offset >= 0);
        if (length < 0 || position < 0) {
          throw new FS.ErrnoError(28);
        }
        if (FS.isClosed(stream)) {
          throw new FS.ErrnoError(8);
        }
        if ((stream.flags & 2097155) === 1) {
          throw new FS.ErrnoError(8);
        }
        if (FS.isDir(stream.node.mode)) {
          throw new FS.ErrnoError(31);
        }
        if (!stream.stream_ops.read) {
          throw new FS.ErrnoError(28);
        }
        var seeking = typeof position != 'undefined';
        if (!seeking) {
          position = stream.position;
        } else if (!stream.seekable) {
          throw new FS.ErrnoError(70);
        }
        var bytesRead = stream.stream_ops.read(stream, buffer, offset, length, position);
        if (!seeking) stream.position += bytesRead;
        return bytesRead;
      },
  write(stream, buffer, offset, length, position, canOwn) {
        assert(offset >= 0);
        if (length < 0 || position < 0) {
          throw new FS.ErrnoError(28);
        }
        if (FS.isClosed(stream)) {
          throw new FS.ErrnoError(8);
        }
        if ((stream.flags & 2097155) === 0) {
          throw new FS.ErrnoError(8);
        }
        if (FS.isDir(stream.node.mode)) {
          throw new FS.ErrnoError(31);
        }
        if (!stream.stream_ops.write) {
          throw new FS.ErrnoError(28);
        }
        if (stream.seekable && stream.flags & 1024) {
          // seek to the end before writing in append mode
          FS.llseek(stream, 0, 2);
        }
        var seeking = typeof position != 'undefined';
        if (!seeking) {
          position = stream.position;
        } else if (!stream.seekable) {
          throw new FS.ErrnoError(70);
        }
        var bytesWritten = stream.stream_ops.write(stream, buffer, offset, length, position, canOwn);
        if (!seeking) stream.position += bytesWritten;
        return bytesWritten;
      },
  allocate(stream, offset, length) {
        if (FS.isClosed(stream)) {
          throw new FS.ErrnoError(8);
        }
        if (offset < 0 || length <= 0) {
          throw new FS.ErrnoError(28);
        }
        if ((stream.flags & 2097155) === 0) {
          throw new FS.ErrnoError(8);
        }
        if (!FS.isFile(stream.node.mode) && !FS.isDir(stream.node.mode)) {
          throw new FS.ErrnoError(43);
        }
        if (!stream.stream_ops.allocate) {
          throw new FS.ErrnoError(138);
        }
        stream.stream_ops.allocate(stream, offset, length);
      },
  mmap(stream, length, position, prot, flags) {
        // User requests writing to file (prot & PROT_WRITE != 0).
        // Checking if we have permissions to write to the file unless
        // MAP_PRIVATE flag is set. According to POSIX spec it is possible
        // to write to file opened in read-only mode with MAP_PRIVATE flag,
        // as all modifications will be visible only in the memory of
        // the current process.
        if ((prot & 2) !== 0
            && (flags & 2) === 0
            && (stream.flags & 2097155) !== 2) {
          throw new FS.ErrnoError(2);
        }
        if ((stream.flags & 2097155) === 1) {
          throw new FS.ErrnoError(2);
        }
        if (!stream.stream_ops.mmap) {
          throw new FS.ErrnoError(43);
        }
        return stream.stream_ops.mmap(stream, length, position, prot, flags);
      },
  msync(stream, buffer, offset, length, mmapFlags) {
        assert(offset >= 0);
        if (!stream.stream_ops.msync) {
          return 0;
        }
        return stream.stream_ops.msync(stream, buffer, offset, length, mmapFlags);
      },
  munmap:(stream) => 0,
  ioctl(stream, cmd, arg) {
        if (!stream.stream_ops.ioctl) {
          throw new FS.ErrnoError(59);
        }
        return stream.stream_ops.ioctl(stream, cmd, arg);
      },
  readFile(path, opts = {}) {
        opts.flags = opts.flags || 0;
        opts.encoding = opts.encoding || 'binary';
        if (opts.encoding !== 'utf8' && opts.encoding !== 'binary') {
          throw new Error(`Invalid encoding type "${opts.encoding}"`);
        }
        var ret;
        var stream = FS.open(path, opts.flags);
        var stat = FS.stat(path);
        var length = stat.size;
        var buf = new Uint8Array(length);
        FS.read(stream, buf, 0, length, 0);
        if (opts.encoding === 'utf8') {
          ret = UTF8ArrayToString(buf, 0);
        } else if (opts.encoding === 'binary') {
          ret = buf;
        }
        FS.close(stream);
        return ret;
      },
  writeFile(path, data, opts = {}) {
        opts.flags = opts.flags || 577;
        var stream = FS.open(path, opts.flags, opts.mode);
        if (typeof data == 'string') {
          var buf = new Uint8Array(lengthBytesUTF8(data)+1);
          var actualNumBytes = stringToUTF8Array(data, buf, 0, buf.length);
          FS.write(stream, buf, 0, actualNumBytes, undefined, opts.canOwn);
        } else if (ArrayBuffer.isView(data)) {
          FS.write(stream, data, 0, data.byteLength, undefined, opts.canOwn);
        } else {
          throw new Error('Unsupported data type');
        }
        FS.close(stream);
      },
  cwd:() => FS.currentPath,
  chdir(path) {
        var lookup = FS.lookupPath(path, { follow: true });
        if (lookup.node === null) {
          throw new FS.ErrnoError(44);
        }
        if (!FS.isDir(lookup.node.mode)) {
          throw new FS.ErrnoError(54);
        }
        var errCode = FS.nodePermissions(lookup.node, 'x');
        if (errCode) {
          throw new FS.ErrnoError(errCode);
        }
        FS.currentPath = lookup.path;
      },
  createDefaultDirectories() {
        FS.mkdir('/tmp');
        FS.mkdir('/home');
        FS.mkdir('/home/web_user');
      },
  createDefaultDevices() {
        // create /dev
        FS.mkdir('/dev');
        // setup /dev/null
        FS.registerDevice(FS.makedev(1, 3), {
          read: () => 0,
          write: (stream, buffer, offset, length, pos) => length,
        });
        FS.mkdev('/dev/null', FS.makedev(1, 3));
        // setup /dev/tty and /dev/tty1
        // stderr needs to print output using err() rather than out()
        // so we register a second tty just for it.
        TTY.register(FS.makedev(5, 0), TTY.default_tty_ops);
        TTY.register(FS.makedev(6, 0), TTY.default_tty1_ops);
        FS.mkdev('/dev/tty', FS.makedev(5, 0));
        FS.mkdev('/dev/tty1', FS.makedev(6, 0));
        // setup /dev/[u]random
        // use a buffer to avoid overhead of individual crypto calls per byte
        var randomBuffer = new Uint8Array(1024), randomLeft = 0;
        var randomByte = () => {
          if (randomLeft === 0) {
            randomLeft = randomFill(randomBuffer).byteLength;
          }
          return randomBuffer[--randomLeft];
        };
        FS.createDevice('/dev', 'random', randomByte);
        FS.createDevice('/dev', 'urandom', randomByte);
        // we're not going to emulate the actual shm device,
        // just create the tmp dirs that reside in it commonly
        FS.mkdir('/dev/shm');
        FS.mkdir('/dev/shm/tmp');
      },
  createSpecialDirectories() {
        // create /proc/self/fd which allows /proc/self/fd/6 => readlink gives the
        // name of the stream for fd 6 (see test_unistd_ttyname)
        FS.mkdir('/proc');
        var proc_self = FS.mkdir('/proc/self');
        FS.mkdir('/proc/self/fd');
        FS.mount({
          mount() {
            var node = FS.createNode(proc_self, 'fd', 16384 | 511 /* 0777 */, 73);
            node.node_ops = {
              lookup(parent, name) {
                var fd = +name;
                var stream = FS.getStreamChecked(fd);
                var ret = {
                  parent: null,
                  mount: { mountpoint: 'fake' },
                  node_ops: { readlink: () => stream.path },
                };
                ret.parent = ret; // make it look like a simple root node
                return ret;
              }
            };
            return node;
          }
        }, {}, '/proc/self/fd');
      },
  createStandardStreams() {
        // TODO deprecate the old functionality of a single
        // input / output callback and that utilizes FS.createDevice
        // and instead require a unique set of stream ops
  
        // by default, we symlink the standard streams to the
        // default tty devices. however, if the standard streams
        // have been overwritten we create a unique device for
        // them instead.
        if (Module['stdin']) {
          FS.createDevice('/dev', 'stdin', Module['stdin']);
        } else {
          FS.symlink('/dev/tty', '/dev/stdin');
        }
        if (Module['stdout']) {
          FS.createDevice('/dev', 'stdout', null, Module['stdout']);
        } else {
          FS.symlink('/dev/tty', '/dev/stdout');
        }
        if (Module['stderr']) {
          FS.createDevice('/dev', 'stderr', null, Module['stderr']);
        } else {
          FS.symlink('/dev/tty1', '/dev/stderr');
        }
  
        // open default streams for the stdin, stdout and stderr devices
        var stdin = FS.open('/dev/stdin', 0);
        var stdout = FS.open('/dev/stdout', 1);
        var stderr = FS.open('/dev/stderr', 1);
        assert(stdin.fd === 0, `invalid handle for stdin (${stdin.fd})`);
        assert(stdout.fd === 1, `invalid handle for stdout (${stdout.fd})`);
        assert(stderr.fd === 2, `invalid handle for stderr (${stderr.fd})`);
      },
  ensureErrnoError() {
        if (FS.ErrnoError) return;
        FS.ErrnoError = /** @this{Object} */ function ErrnoError(errno, node) {
          // We set the `name` property to be able to identify `FS.ErrnoError`
          // - the `name` is a standard ECMA-262 property of error objects. Kind of good to have it anyway.
          // - when using PROXYFS, an error can come from an underlying FS
          // as different FS objects have their own FS.ErrnoError each,
          // the test `err instanceof FS.ErrnoError` won't detect an error coming from another filesystem, causing bugs.
          // we'll use the reliable test `err.name == "ErrnoError"` instead
          this.name = 'ErrnoError';
          this.node = node;
          this.setErrno = /** @this{Object} */ function(errno) {
            this.errno = errno;
            for (var key in ERRNO_CODES) {
              if (ERRNO_CODES[key] === errno) {
                this.code = key;
                break;
              }
            }
          };
          this.setErrno(errno);
          this.message = ERRNO_MESSAGES[errno];
  
          // Try to get a maximally helpful stack trace. On Node.js, getting Error.stack
          // now ensures it shows what we want.
          if (this.stack) {
            // Define the stack property for Node.js 4, which otherwise errors on the next line.
            Object.defineProperty(this, "stack", { value: (new Error).stack, writable: true });
            this.stack = demangleAll(this.stack);
          }
        };
        FS.ErrnoError.prototype = new Error();
        FS.ErrnoError.prototype.constructor = FS.ErrnoError;
        // Some errors may happen quite a bit, to avoid overhead we reuse them (and suffer a lack of stack info)
        [44].forEach((code) => {
          FS.genericErrors[code] = new FS.ErrnoError(code);
          FS.genericErrors[code].stack = '<generic error, no stack>';
        });
      },
  staticInit() {
        FS.ensureErrnoError();
  
        FS.nameTable = new Array(4096);
  
        FS.mount(MEMFS, {}, '/');
  
        FS.createDefaultDirectories();
        FS.createDefaultDevices();
        FS.createSpecialDirectories();
  
        FS.filesystems = {
          'MEMFS': MEMFS,
          'NODEFS': NODEFS,
        };
      },
  init(input, output, error) {
        assert(!FS.init.initialized, 'FS.init was previously called. If you want to initialize later with custom parameters, remove any earlier calls (note that one is automatically added to the generated code)');
        FS.init.initialized = true;
  
        FS.ensureErrnoError();
  
        // Allow Module.stdin etc. to provide defaults, if none explicitly passed to us here
        Module['stdin'] = input || Module['stdin'];
        Module['stdout'] = output || Module['stdout'];
        Module['stderr'] = error || Module['stderr'];
  
        FS.createStandardStreams();
      },
  quit() {
        FS.init.initialized = false;
        // force-flush all streams, so we get musl std streams printed out
        _fflush(0);
        // close all of our streams
        for (var i = 0; i < FS.streams.length; i++) {
          var stream = FS.streams[i];
          if (!stream) {
            continue;
          }
          FS.close(stream);
        }
      },
  findObject(path, dontResolveLastLink) {
        var ret = FS.analyzePath(path, dontResolveLastLink);
        if (!ret.exists) {
          return null;
        }
        return ret.object;
      },
  analyzePath(path, dontResolveLastLink) {
        // operate from within the context of the symlink's target
        try {
          var lookup = FS.lookupPath(path, { follow: !dontResolveLastLink });
          path = lookup.path;
        } catch (e) {
        }
        var ret = {
          isRoot: false, exists: false, error: 0, name: null, path: null, object: null,
          parentExists: false, parentPath: null, parentObject: null
        };
        try {
          var lookup = FS.lookupPath(path, { parent: true });
          ret.parentExists = true;
          ret.parentPath = lookup.path;
          ret.parentObject = lookup.node;
          ret.name = PATH.basename(path);
          lookup = FS.lookupPath(path, { follow: !dontResolveLastLink });
          ret.exists = true;
          ret.path = lookup.path;
          ret.object = lookup.node;
          ret.name = lookup.node.name;
          ret.isRoot = lookup.path === '/';
        } catch (e) {
          ret.error = e.errno;
        };
        return ret;
      },
  createPath(parent, path, canRead, canWrite) {
        parent = typeof parent == 'string' ? parent : FS.getPath(parent);
        var parts = path.split('/').reverse();
        while (parts.length) {
          var part = parts.pop();
          if (!part) continue;
          var current = PATH.join2(parent, part);
          try {
            FS.mkdir(current);
          } catch (e) {
            // ignore EEXIST
          }
          parent = current;
        }
        return current;
      },
  createFile(parent, name, properties, canRead, canWrite) {
        var path = PATH.join2(typeof parent == 'string' ? parent : FS.getPath(parent), name);
        var mode = FS_getMode(canRead, canWrite);
        return FS.create(path, mode);
      },
  createDataFile(parent, name, data, canRead, canWrite, canOwn) {
        var path = name;
        if (parent) {
          parent = typeof parent == 'string' ? parent : FS.getPath(parent);
          path = name ? PATH.join2(parent, name) : parent;
        }
        var mode = FS_getMode(canRead, canWrite);
        var node = FS.create(path, mode);
        if (data) {
          if (typeof data == 'string') {
            var arr = new Array(data.length);
            for (var i = 0, len = data.length; i < len; ++i) arr[i] = data.charCodeAt(i);
            data = arr;
          }
          // make sure we can write to the file
          FS.chmod(node, mode | 146);
          var stream = FS.open(node, 577);
          FS.write(stream, data, 0, data.length, 0, canOwn);
          FS.close(stream);
          FS.chmod(node, mode);
        }
        return node;
      },
  createDevice(parent, name, input, output) {
        var path = PATH.join2(typeof parent == 'string' ? parent : FS.getPath(parent), name);
        var mode = FS_getMode(!!input, !!output);
        if (!FS.createDevice.major) FS.createDevice.major = 64;
        var dev = FS.makedev(FS.createDevice.major++, 0);
        // Create a fake device that a set of stream ops to emulate
        // the old behavior.
        FS.registerDevice(dev, {
          open(stream) {
            stream.seekable = false;
          },
          close(stream) {
            // flush any pending line data
            if (output && output.buffer && output.buffer.length) {
              output(10);
            }
          },
          read(stream, buffer, offset, length, pos /* ignored */) {
            var bytesRead = 0;
            for (var i = 0; i < length; i++) {
              var result;
              try {
                result = input();
              } catch (e) {
                throw new FS.ErrnoError(29);
              }
              if (result === undefined && bytesRead === 0) {
                throw new FS.ErrnoError(6);
              }
              if (result === null || result === undefined) break;
              bytesRead++;
              buffer[offset+i] = result;
            }
            if (bytesRead) {
              stream.node.timestamp = Date.now();
            }
            return bytesRead;
          },
          write(stream, buffer, offset, length, pos) {
            for (var i = 0; i < length; i++) {
              try {
                output(buffer[offset+i]);
              } catch (e) {
                throw new FS.ErrnoError(29);
              }
            }
            if (length) {
              stream.node.timestamp = Date.now();
            }
            return i;
          }
        });
        return FS.mkdev(path, mode, dev);
      },
  forceLoadFile(obj) {
        if (obj.isDevice || obj.isFolder || obj.link || obj.contents) return true;
        if (typeof XMLHttpRequest != 'undefined') {
          throw new Error("Lazy loading should have been performed (contents set) in createLazyFile, but it was not. Lazy loading only works in web workers. Use --embed-file or --preload-file in emcc on the main thread.");
        } else if (read_) {
          // Command-line.
          try {
            // WARNING: Can't read binary files in V8's d8 or tracemonkey's js, as
            //          read() will try to parse UTF8.
            obj.contents = intArrayFromString(read_(obj.url), true);
            obj.usedBytes = obj.contents.length;
          } catch (e) {
            throw new FS.ErrnoError(29);
          }
        } else {
          throw new Error('Cannot load without read() or XMLHttpRequest.');
        }
      },
  createLazyFile(parent, name, url, canRead, canWrite) {
        // Lazy chunked Uint8Array (implements get and length from Uint8Array). Actual getting is abstracted away for eventual reuse.
        /** @constructor */
        function LazyUint8Array() {
          this.lengthKnown = false;
          this.chunks = []; // Loaded chunks. Index is the chunk number
        }
        LazyUint8Array.prototype.get = /** @this{Object} */ function LazyUint8Array_get(idx) {
          if (idx > this.length-1 || idx < 0) {
            return undefined;
          }
          var chunkOffset = idx % this.chunkSize;
          var chunkNum = (idx / this.chunkSize)|0;
          return this.getter(chunkNum)[chunkOffset];
        };
        LazyUint8Array.prototype.setDataGetter = function LazyUint8Array_setDataGetter(getter) {
          this.getter = getter;
        };
        LazyUint8Array.prototype.cacheLength = function LazyUint8Array_cacheLength() {
          // Find length
          var xhr = new XMLHttpRequest();
          xhr.open('HEAD', url, false);
          xhr.send(null);
          if (!(xhr.status >= 200 && xhr.status < 300 || xhr.status === 304)) throw new Error("Couldn't load " + url + ". Status: " + xhr.status);
          var datalength = Number(xhr.getResponseHeader("Content-length"));
          var header;
          var hasByteServing = (header = xhr.getResponseHeader("Accept-Ranges")) && header === "bytes";
          var usesGzip = (header = xhr.getResponseHeader("Content-Encoding")) && header === "gzip";
  
          var chunkSize = 1024*1024; // Chunk size in bytes
  
          if (!hasByteServing) chunkSize = datalength;
  
          // Function to get a range from the remote URL.
          var doXHR = (from, to) => {
            if (from > to) throw new Error("invalid range (" + from + ", " + to + ") or no bytes requested!");
            if (to > datalength-1) throw new Error("only " + datalength + " bytes available! programmer error!");
  
            // TODO: Use mozResponseArrayBuffer, responseStream, etc. if available.
            var xhr = new XMLHttpRequest();
            xhr.open('GET', url, false);
            if (datalength !== chunkSize) xhr.setRequestHeader("Range", "bytes=" + from + "-" + to);
  
            // Some hints to the browser that we want binary data.
            xhr.responseType = 'arraybuffer';
            if (xhr.overrideMimeType) {
              xhr.overrideMimeType('text/plain; charset=x-user-defined');
            }
  
            xhr.send(null);
            if (!(xhr.status >= 200 && xhr.status < 300 || xhr.status === 304)) throw new Error("Couldn't load " + url + ". Status: " + xhr.status);
            if (xhr.response !== undefined) {
              return new Uint8Array(/** @type{Array<number>} */(xhr.response || []));
            }
            return intArrayFromString(xhr.responseText || '', true);
          };
          var lazyArray = this;
          lazyArray.setDataGetter((chunkNum) => {
            var start = chunkNum * chunkSize;
            var end = (chunkNum+1) * chunkSize - 1; // including this byte
            end = Math.min(end, datalength-1); // if datalength-1 is selected, this is the last block
            if (typeof lazyArray.chunks[chunkNum] == 'undefined') {
              lazyArray.chunks[chunkNum] = doXHR(start, end);
            }
            if (typeof lazyArray.chunks[chunkNum] == 'undefined') throw new Error('doXHR failed!');
            return lazyArray.chunks[chunkNum];
          });
  
          if (usesGzip || !datalength) {
            // if the server uses gzip or doesn't supply the length, we have to download the whole file to get the (uncompressed) length
            chunkSize = datalength = 1; // this will force getter(0)/doXHR do download the whole file
            datalength = this.getter(0).length;
            chunkSize = datalength;
            out("LazyFiles on gzip forces download of the whole file when length is accessed");
          }
  
          this._length = datalength;
          this._chunkSize = chunkSize;
          this.lengthKnown = true;
        };
        if (typeof XMLHttpRequest != 'undefined') {
          if (!ENVIRONMENT_IS_WORKER) throw 'Cannot do synchronous binary XHRs outside webworkers in modern browsers. Use --embed-file or --preload-file in emcc';
          var lazyArray = new LazyUint8Array();
          Object.defineProperties(lazyArray, {
            length: {
              get: /** @this{Object} */ function() {
                if (!this.lengthKnown) {
                  this.cacheLength();
                }
                return this._length;
              }
            },
            chunkSize: {
              get: /** @this{Object} */ function() {
                if (!this.lengthKnown) {
                  this.cacheLength();
                }
                return this._chunkSize;
              }
            }
          });
  
          var properties = { isDevice: false, contents: lazyArray };
        } else {
          var properties = { isDevice: false, url: url };
        }
  
        var node = FS.createFile(parent, name, properties, canRead, canWrite);
        // This is a total hack, but I want to get this lazy file code out of the
        // core of MEMFS. If we want to keep this lazy file concept I feel it should
        // be its own thin LAZYFS proxying calls to MEMFS.
        if (properties.contents) {
          node.contents = properties.contents;
        } else if (properties.url) {
          node.contents = null;
          node.url = properties.url;
        }
        // Add a function that defers querying the file size until it is asked the first time.
        Object.defineProperties(node, {
          usedBytes: {
            get: /** @this {FSNode} */ function() { return this.contents.length; }
          }
        });
        // override each stream op with one that tries to force load the lazy file first
        var stream_ops = {};
        var keys = Object.keys(node.stream_ops);
        keys.forEach((key) => {
          var fn = node.stream_ops[key];
          stream_ops[key] = function forceLoadLazyFile() {
            FS.forceLoadFile(node);
            return fn.apply(null, arguments);
          };
        });
        function writeChunks(stream, buffer, offset, length, position) {
          var contents = stream.node.contents;
          if (position >= contents.length)
            return 0;
          var size = Math.min(contents.length - position, length);
          assert(size >= 0);
          if (contents.slice) { // normal array
            for (var i = 0; i < size; i++) {
              buffer[offset + i] = contents[position + i];
            }
          } else {
            for (var i = 0; i < size; i++) { // LazyUint8Array from sync binary XHR
              buffer[offset + i] = contents.get(position + i);
            }
          }
          return size;
        }
        // use a custom read function
        stream_ops.read = (stream, buffer, offset, length, position) => {
          FS.forceLoadFile(node);
          return writeChunks(stream, buffer, offset, length, position)
        };
        // use a custom mmap function
        stream_ops.mmap = (stream, length, position, prot, flags) => {
          FS.forceLoadFile(node);
          var ptr = mmapAlloc(length);
          if (!ptr) {
            throw new FS.ErrnoError(48);
          }
          writeChunks(stream, HEAP8, ptr, length, position);
          return { ptr, allocated: true };
        };
        node.stream_ops = stream_ops;
        return node;
      },
  absolutePath() {
        abort('FS.absolutePath has been removed; use PATH_FS.resolve instead');
      },
  createFolder() {
        abort('FS.createFolder has been removed; use FS.mkdir instead');
      },
  createLink() {
        abort('FS.createLink has been removed; use FS.symlink instead');
      },
  joinPath() {
        abort('FS.joinPath has been removed; use PATH.join instead');
      },
  mmapAlloc() {
        abort('FS.mmapAlloc has been replaced by the top level function mmapAlloc');
      },
  standardizePath() {
        abort('FS.standardizePath has been removed; use PATH.normalize instead');
      },
  };
  
  var SYSCALLS = {
  DEFAULT_POLLMASK:5,
  calculateAt(dirfd, path, allowEmpty) {
        if (PATH.isAbs(path)) {
          return path;
        }
        // relative path
        var dir;
        if (dirfd === -100) {
          dir = FS.cwd();
        } else {
          var dirstream = SYSCALLS.getStreamFromFD(dirfd);
          dir = dirstream.path;
        }
        if (path.length == 0) {
          if (!allowEmpty) {
            throw new FS.ErrnoError(44);;
          }
          return dir;
        }
        return PATH.join2(dir, path);
      },
  doStat(func, path, buf) {
        try {
          var stat = func(path);
        } catch (e) {
          if (e && e.node && PATH.normalize(path) !== PATH.normalize(FS.getPath(e.node))) {
            // an error occurred while trying to look up the path; we should just report ENOTDIR
            return -54;
          }
          throw e;
        }
        HEAP32[((buf)>>2)] = stat.dev;
        HEAP32[(((buf)+(4))>>2)] = stat.mode;
        HEAPU32[(((buf)+(8))>>2)] = stat.nlink;
        HEAP32[(((buf)+(12))>>2)] = stat.uid;
        HEAP32[(((buf)+(16))>>2)] = stat.gid;
        HEAP32[(((buf)+(20))>>2)] = stat.rdev;
        HEAP64[(((buf)+(24))>>3)] = BigInt(stat.size);
        HEAP32[(((buf)+(32))>>2)] = 4096;
        HEAP32[(((buf)+(36))>>2)] = stat.blocks;
        var atime = stat.atime.getTime();
        var mtime = stat.mtime.getTime();
        var ctime = stat.ctime.getTime();
        HEAP64[(((buf)+(40))>>3)] = BigInt(Math.floor(atime / 1000));
        HEAPU32[(((buf)+(48))>>2)] = (atime % 1000) * 1000;
        HEAP64[(((buf)+(56))>>3)] = BigInt(Math.floor(mtime / 1000));
        HEAPU32[(((buf)+(64))>>2)] = (mtime % 1000) * 1000;
        HEAP64[(((buf)+(72))>>3)] = BigInt(Math.floor(ctime / 1000));
        HEAPU32[(((buf)+(80))>>2)] = (ctime % 1000) * 1000;
        HEAP64[(((buf)+(88))>>3)] = BigInt(stat.ino);
        return 0;
      },
  doMsync(addr, stream, len, flags, offset) {
        if (!FS.isFile(stream.node.mode)) {
          throw new FS.ErrnoError(43);
        }
        if (flags & 2) {
          // MAP_PRIVATE calls need not to be synced back to underlying fs
          return 0;
        }
        var buffer = HEAPU8.slice(addr, addr + len);
        FS.msync(stream, buffer, offset, len, flags);
      },
  varargs:undefined,
  get() {
        assert(SYSCALLS.varargs != undefined);
        var ret = HEAP32[((SYSCALLS.varargs)>>2)];
        SYSCALLS.varargs += 4;
        return ret;
      },
  getp() { return SYSCALLS.get() },
  getStr(ptr) {
        var ret = UTF8ToString(ptr);
        return ret;
      },
  getStreamFromFD(fd) {
        var stream = FS.getStreamChecked(fd);
        return stream;
      },
  };
  function ___syscall__newselect(nfds, readfds, writefds, exceptfds, timeout) {
  try {
  
      // readfds are supported,
      // writefds checks socket open status
      // exceptfds not supported
      // timeout is always 0 - fully async
      assert(nfds <= 64, 'nfds must be less than or equal to 64');  // fd sets have 64 bits // TODO: this could be 1024 based on current musl headers
      assert(!exceptfds, 'exceptfds not supported');
  
      var total = 0;
  
      var srcReadLow = (readfds ? HEAP32[((readfds)>>2)] : 0),
          srcReadHigh = (readfds ? HEAP32[(((readfds)+(4))>>2)] : 0);
      var srcWriteLow = (writefds ? HEAP32[((writefds)>>2)] : 0),
          srcWriteHigh = (writefds ? HEAP32[(((writefds)+(4))>>2)] : 0);
      var srcExceptLow = (exceptfds ? HEAP32[((exceptfds)>>2)] : 0),
          srcExceptHigh = (exceptfds ? HEAP32[(((exceptfds)+(4))>>2)] : 0);
  
      var dstReadLow = 0,
          dstReadHigh = 0;
      var dstWriteLow = 0,
          dstWriteHigh = 0;
      var dstExceptLow = 0,
          dstExceptHigh = 0;
  
      var allLow = (readfds ? HEAP32[((readfds)>>2)] : 0) |
                   (writefds ? HEAP32[((writefds)>>2)] : 0) |
                   (exceptfds ? HEAP32[((exceptfds)>>2)] : 0);
      var allHigh = (readfds ? HEAP32[(((readfds)+(4))>>2)] : 0) |
                    (writefds ? HEAP32[(((writefds)+(4))>>2)] : 0) |
                    (exceptfds ? HEAP32[(((exceptfds)+(4))>>2)] : 0);
  
      var check = function(fd, low, high, val) {
        return (fd < 32 ? (low & val) : (high & val));
      };
  
      for (var fd = 0; fd < nfds; fd++) {
        var mask = 1 << (fd % 32);
        if (!(check(fd, allLow, allHigh, mask))) {
          continue;  // index isn't in the set
        }
  
        var stream = SYSCALLS.getStreamFromFD(fd);
  
        var flags = SYSCALLS.DEFAULT_POLLMASK;
  
        if (stream.stream_ops.poll) {
          var timeoutInMillis = -1;
          if (timeout) {
            var tv_sec = (readfds ? HEAP32[((timeout)>>2)] : 0),
                tv_usec = (readfds ? HEAP32[(((timeout)+(8))>>2)] : 0);
            timeoutInMillis = (tv_sec + tv_usec / 1000000) * 1000;
          }
          flags = stream.stream_ops.poll(stream, timeoutInMillis);
        }
  
        if ((flags & 1) && check(fd, srcReadLow, srcReadHigh, mask)) {
          fd < 32 ? (dstReadLow = dstReadLow | mask) : (dstReadHigh = dstReadHigh | mask);
          total++;
        }
        if ((flags & 4) && check(fd, srcWriteLow, srcWriteHigh, mask)) {
          fd < 32 ? (dstWriteLow = dstWriteLow | mask) : (dstWriteHigh = dstWriteHigh | mask);
          total++;
        }
        if ((flags & 2) && check(fd, srcExceptLow, srcExceptHigh, mask)) {
          fd < 32 ? (dstExceptLow = dstExceptLow | mask) : (dstExceptHigh = dstExceptHigh | mask);
          total++;
        }
      }
  
      if (readfds) {
        HEAP32[((readfds)>>2)] = dstReadLow;
        HEAP32[(((readfds)+(4))>>2)] = dstReadHigh;
      }
      if (writefds) {
        HEAP32[((writefds)>>2)] = dstWriteLow;
        HEAP32[(((writefds)+(4))>>2)] = dstWriteHigh;
      }
      if (exceptfds) {
        HEAP32[((exceptfds)>>2)] = dstExceptLow;
        HEAP32[(((exceptfds)+(4))>>2)] = dstExceptHigh;
      }
  
      return total;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  var SOCKFS = {
  mount(mount) {
        // If Module['websocket'] has already been defined (e.g. for configuring
        // the subprotocol/url) use that, if not initialise it to a new object.
        Module['websocket'] = (Module['websocket'] &&
                               ('object' === typeof Module['websocket'])) ? Module['websocket'] : {};
  
        // Add the Event registration mechanism to the exported websocket configuration
        // object so we can register network callbacks from native JavaScript too.
        // For more documentation see system/include/emscripten/emscripten.h
        Module['websocket']._callbacks = {};
        Module['websocket']['on'] = /** @this{Object} */ function(event, callback) {
          if ('function' === typeof callback) {
            this._callbacks[event] = callback;
          }
          return this;
        };
  
        Module['websocket'].emit = /** @this{Object} */ function(event, param) {
          if ('function' === typeof this._callbacks[event]) {
            this._callbacks[event].call(this, param);
          }
        };
  
        // If debug is enabled register simple default logging callbacks for each Event.
  
        return FS.createNode(null, '/', 16384 | 511 /* 0777 */, 0);
      },
  createSocket(family, type, protocol) {
        type &= ~526336; // Some applications may pass it; it makes no sense for a single process.
        var streaming = type == 1;
        if (streaming && protocol && protocol != 6) {
          throw new FS.ErrnoError(66); // if SOCK_STREAM, must be tcp or 0.
        }
  
        // create our internal socket structure
        var sock = {
          family,
          type,
          protocol,
          server: null,
          error: null, // Used in getsockopt for SOL_SOCKET/SO_ERROR test
          peers: {},
          pending: [],
          recv_queue: [],
          sock_ops: SOCKFS.websocket_sock_ops
        };
  
        // create the filesystem node to store the socket structure
        var name = SOCKFS.nextname();
        var node = FS.createNode(SOCKFS.root, name, 49152, 0);
        node.sock = sock;
  
        // and the wrapping stream that enables library functions such
        // as read and write to indirectly interact with the socket
        var stream = FS.createStream({
          path: name,
          node,
          flags: 2,
          seekable: false,
          stream_ops: SOCKFS.stream_ops
        });
  
        // map the new stream to the socket structure (sockets have a 1:1
        // relationship with a stream)
        sock.stream = stream;
  
        return sock;
      },
  getSocket(fd) {
        var stream = FS.getStream(fd);
        if (!stream || !FS.isSocket(stream.node.mode)) {
          return null;
        }
        return stream.node.sock;
      },
  stream_ops:{
  poll(stream) {
          var sock = stream.node.sock;
          return sock.sock_ops.poll(sock);
        },
  ioctl(stream, request, varargs) {
          var sock = stream.node.sock;
          return sock.sock_ops.ioctl(sock, request, varargs);
        },
  read(stream, buffer, offset, length, position /* ignored */) {
          var sock = stream.node.sock;
          var msg = sock.sock_ops.recvmsg(sock, length);
          if (!msg) {
            // socket is closed
            return 0;
          }
          buffer.set(msg.buffer, offset);
          return msg.buffer.length;
        },
  write(stream, buffer, offset, length, position /* ignored */) {
          var sock = stream.node.sock;
          return sock.sock_ops.sendmsg(sock, buffer, offset, length);
        },
  close(stream) {
          var sock = stream.node.sock;
          sock.sock_ops.close(sock);
        },
  },
  nextname() {
        if (!SOCKFS.nextname.current) {
          SOCKFS.nextname.current = 0;
        }
        return 'socket[' + (SOCKFS.nextname.current++) + ']';
      },
  websocket_sock_ops:{
  createPeer(sock, addr, port) {
          var ws;
  
          if (typeof addr == 'object') {
            ws = addr;
            addr = null;
            port = null;
          }
  
          if (ws) {
            // for sockets that've already connected (e.g. we're the server)
            // we can inspect the _socket property for the address
            if (ws._socket) {
              addr = ws._socket.remoteAddress;
              port = ws._socket.remotePort;
            }
            // if we're just now initializing a connection to the remote,
            // inspect the url property
            else {
              var result = /ws[s]?:\/\/([^:]+):(\d+)/.exec(ws.url);
              if (!result) {
                throw new Error('WebSocket URL must be in the format ws(s)://address:port');
              }
              addr = result[1];
              port = parseInt(result[2], 10);
            }
          } else {
            // create the actual websocket object and connect
            try {
              // runtimeConfig gets set to true if WebSocket runtime configuration is available.
              var runtimeConfig = (Module['websocket'] && ('object' === typeof Module['websocket']));
  
              // The default value is 'ws://' the replace is needed because the compiler replaces '//' comments with '#'
              // comments without checking context, so we'd end up with ws:#, the replace swaps the '#' for '//' again.
              var url = 'ws:#'.replace('#', '//');
  
              if (runtimeConfig) {
                if ('string' === typeof Module['websocket']['url']) {
                  url = Module['websocket']['url']; // Fetch runtime WebSocket URL config.
                }
              }
  
              if (url === 'ws://' || url === 'wss://') { // Is the supplied URL config just a prefix, if so complete it.
                var parts = addr.split('/');
                url = url + parts[0] + ":" + port + "/" + parts.slice(1).join('/');
              }
  
              // Make the WebSocket subprotocol (Sec-WebSocket-Protocol) default to binary if no configuration is set.
              var subProtocols = 'binary'; // The default value is 'binary'
  
              if (runtimeConfig) {
                if ('string' === typeof Module['websocket']['subprotocol']) {
                  subProtocols = Module['websocket']['subprotocol']; // Fetch runtime WebSocket subprotocol config.
                }
              }
  
              // The default WebSocket options
              var opts = undefined;
  
              if (subProtocols !== 'null') {
                // The regex trims the string (removes spaces at the beginning and end, then splits the string by
                // <any space>,<any space> into an Array. Whitespace removal is important for Websockify and ws.
                subProtocols = subProtocols.replace(/^ +| +$/g,"").split(/ *, */);
  
                opts = subProtocols;
              }
  
              // some webservers (azure) does not support subprotocol header
              if (runtimeConfig && null === Module['websocket']['subprotocol']) {
                subProtocols = 'null';
                opts = undefined;
              }
  
              // If node we use the ws library.
              var WebSocketConstructor;
              if (ENVIRONMENT_IS_NODE) {
                WebSocketConstructor = /** @type{(typeof WebSocket)} */(require('ws'));
              } else
              {
                WebSocketConstructor = WebSocket;
              }
              ws = new WebSocketConstructor(url, opts);
              ws.binaryType = 'arraybuffer';
            } catch (e) {
              throw new FS.ErrnoError(23);
            }
          }
  
          var peer = {
            addr,
            port,
            socket: ws,
            dgram_send_queue: []
          };
  
          SOCKFS.websocket_sock_ops.addPeer(sock, peer);
          SOCKFS.websocket_sock_ops.handlePeerEvents(sock, peer);
  
          // if this is a bound dgram socket, send the port number first to allow
          // us to override the ephemeral port reported to us by remotePort on the
          // remote end.
          if (sock.type === 2 && typeof sock.sport != 'undefined') {
            peer.dgram_send_queue.push(new Uint8Array([
                255, 255, 255, 255,
                'p'.charCodeAt(0), 'o'.charCodeAt(0), 'r'.charCodeAt(0), 't'.charCodeAt(0),
                ((sock.sport & 0xff00) >> 8) , (sock.sport & 0xff)
            ]));
          }
  
          return peer;
        },
  getPeer(sock, addr, port) {
          return sock.peers[addr + ':' + port];
        },
  addPeer(sock, peer) {
          sock.peers[peer.addr + ':' + peer.port] = peer;
        },
  removePeer(sock, peer) {
          delete sock.peers[peer.addr + ':' + peer.port];
        },
  handlePeerEvents(sock, peer) {
          var first = true;
  
          var handleOpen = function () {
  
            Module['websocket'].emit('open', sock.stream.fd);
  
            try {
              var queued = peer.dgram_send_queue.shift();
              while (queued) {
                peer.socket.send(queued);
                queued = peer.dgram_send_queue.shift();
              }
            } catch (e) {
              // not much we can do here in the way of proper error handling as we've already
              // lied and said this data was sent. shut it down.
              peer.socket.close();
            }
          };
  
          function handleMessage(data) {
            if (typeof data == 'string') {
              var encoder = new TextEncoder(); // should be utf-8
              data = encoder.encode(data); // make a typed array from the string
            } else {
              assert(data.byteLength !== undefined); // must receive an ArrayBuffer
              if (data.byteLength == 0) {
                // An empty ArrayBuffer will emit a pseudo disconnect event
                // as recv/recvmsg will return zero which indicates that a socket
                // has performed a shutdown although the connection has not been disconnected yet.
                return;
              }
              data = new Uint8Array(data); // make a typed array view on the array buffer
            }
  
            // if this is the port message, override the peer's port with it
            var wasfirst = first;
            first = false;
            if (wasfirst &&
                data.length === 10 &&
                data[0] === 255 && data[1] === 255 && data[2] === 255 && data[3] === 255 &&
                data[4] === 'p'.charCodeAt(0) && data[5] === 'o'.charCodeAt(0) && data[6] === 'r'.charCodeAt(0) && data[7] === 't'.charCodeAt(0)) {
              // update the peer's port and it's key in the peer map
              var newport = ((data[8] << 8) | data[9]);
              SOCKFS.websocket_sock_ops.removePeer(sock, peer);
              peer.port = newport;
              SOCKFS.websocket_sock_ops.addPeer(sock, peer);
              return;
            }
  
            sock.recv_queue.push({ addr: peer.addr, port: peer.port, data: data });
            Module['websocket'].emit('message', sock.stream.fd);
          };
  
          if (ENVIRONMENT_IS_NODE) {
            peer.socket.on('open', handleOpen);
            peer.socket.on('message', function(data, isBinary) {
              if (!isBinary) {
                return;
              }
              handleMessage((new Uint8Array(data)).buffer); // copy from node Buffer -> ArrayBuffer
            });
            peer.socket.on('close', function() {
              Module['websocket'].emit('close', sock.stream.fd);
            });
            peer.socket.on('error', function(error) {
              // Although the ws library may pass errors that may be more descriptive than
              // ECONNREFUSED they are not necessarily the expected error code e.g.
              // ENOTFOUND on getaddrinfo seems to be node.js specific, so using ECONNREFUSED
              // is still probably the most useful thing to do.
              sock.error = 14; // Used in getsockopt for SOL_SOCKET/SO_ERROR test.
              Module['websocket'].emit('error', [sock.stream.fd, sock.error, 'ECONNREFUSED: Connection refused']);
              // don't throw
            });
          } else {
            peer.socket.onopen = handleOpen;
            peer.socket.onclose = function() {
              Module['websocket'].emit('close', sock.stream.fd);
            };
            peer.socket.onmessage = function peer_socket_onmessage(event) {
              handleMessage(event.data);
            };
            peer.socket.onerror = function(error) {
              // The WebSocket spec only allows a 'simple event' to be thrown on error,
              // so we only really know as much as ECONNREFUSED.
              sock.error = 14; // Used in getsockopt for SOL_SOCKET/SO_ERROR test.
              Module['websocket'].emit('error', [sock.stream.fd, sock.error, 'ECONNREFUSED: Connection refused']);
            };
          }
        },
  poll(sock) {
          if (sock.type === 1 && sock.server) {
            // listen sockets should only say they're available for reading
            // if there are pending clients.
            return sock.pending.length ? (64 | 1) : 0;
          }
  
          var mask = 0;
          var dest = sock.type === 1 ?  // we only care about the socket state for connection-based sockets
            SOCKFS.websocket_sock_ops.getPeer(sock, sock.daddr, sock.dport) :
            null;
  
          if (sock.recv_queue.length ||
              !dest ||  // connection-less sockets are always ready to read
              (dest && dest.socket.readyState === dest.socket.CLOSING) ||
              (dest && dest.socket.readyState === dest.socket.CLOSED)) {  // let recv return 0 once closed
            mask |= (64 | 1);
          }
  
          if (!dest ||  // connection-less sockets are always ready to write
              (dest && dest.socket.readyState === dest.socket.OPEN)) {
            mask |= 4;
          }
  
          if ((dest && dest.socket.readyState === dest.socket.CLOSING) ||
              (dest && dest.socket.readyState === dest.socket.CLOSED)) {
            mask |= 16;
          }
  
          return mask;
        },
  ioctl(sock, request, arg) {
          switch (request) {
            case 21531:
              var bytes = 0;
              if (sock.recv_queue.length) {
                bytes = sock.recv_queue[0].data.length;
              }
              HEAP32[((arg)>>2)] = bytes;
              return 0;
            default:
              return 28;
          }
        },
  close(sock) {
          // if we've spawned a listen server, close it
          if (sock.server) {
            try {
              sock.server.close();
            } catch (e) {
            }
            sock.server = null;
          }
          // close any peer connections
          var peers = Object.keys(sock.peers);
          for (var i = 0; i < peers.length; i++) {
            var peer = sock.peers[peers[i]];
            try {
              peer.socket.close();
            } catch (e) {
            }
            SOCKFS.websocket_sock_ops.removePeer(sock, peer);
          }
          return 0;
        },
  bind(sock, addr, port) {
          if (typeof sock.saddr != 'undefined' || typeof sock.sport != 'undefined') {
            throw new FS.ErrnoError(28);  // already bound
          }
          sock.saddr = addr;
          sock.sport = port;
          // in order to emulate dgram sockets, we need to launch a listen server when
          // binding on a connection-less socket
          // note: this is only required on the server side
          if (sock.type === 2) {
            // close the existing server if it exists
            if (sock.server) {
              sock.server.close();
              sock.server = null;
            }
            // swallow error operation not supported error that occurs when binding in the
            // browser where this isn't supported
            try {
              sock.sock_ops.listen(sock, 0);
            } catch (e) {
              if (!(e.name === 'ErrnoError')) throw e;
              if (e.errno !== 138) throw e;
            }
          }
        },
  connect(sock, addr, port) {
          if (sock.server) {
            throw new FS.ErrnoError(138);
          }
  
          // TODO autobind
          // if (!sock.addr && sock.type == 2) {
          // }
  
          // early out if we're already connected / in the middle of connecting
          if (typeof sock.daddr != 'undefined' && typeof sock.dport != 'undefined') {
            var dest = SOCKFS.websocket_sock_ops.getPeer(sock, sock.daddr, sock.dport);
            if (dest) {
              if (dest.socket.readyState === dest.socket.CONNECTING) {
                throw new FS.ErrnoError(7);
              } else {
                throw new FS.ErrnoError(30);
              }
            }
          }
  
          // add the socket to our peer list and set our
          // destination address / port to match
          var peer = SOCKFS.websocket_sock_ops.createPeer(sock, addr, port);
          sock.daddr = peer.addr;
          sock.dport = peer.port;
  
          // always "fail" in non-blocking mode
          throw new FS.ErrnoError(26);
        },
  listen(sock, backlog) {
          if (!ENVIRONMENT_IS_NODE) {
            throw new FS.ErrnoError(138);
          }
          if (sock.server) {
             throw new FS.ErrnoError(28);  // already listening
          }
          var WebSocketServer = require('ws').Server;
          var host = sock.saddr;
          sock.server = new WebSocketServer({
            host,
            port: sock.sport
            // TODO support backlog
          });
          Module['websocket'].emit('listen', sock.stream.fd); // Send Event with listen fd.
  
          sock.server.on('connection', function(ws) {
            if (sock.type === 1) {
              var newsock = SOCKFS.createSocket(sock.family, sock.type, sock.protocol);
  
              // create a peer on the new socket
              var peer = SOCKFS.websocket_sock_ops.createPeer(newsock, ws);
              newsock.daddr = peer.addr;
              newsock.dport = peer.port;
  
              // push to queue for accept to pick up
              sock.pending.push(newsock);
              Module['websocket'].emit('connection', newsock.stream.fd);
            } else {
              // create a peer on the listen socket so calling sendto
              // with the listen socket and an address will resolve
              // to the correct client
              SOCKFS.websocket_sock_ops.createPeer(sock, ws);
              Module['websocket'].emit('connection', sock.stream.fd);
            }
          });
          sock.server.on('close', function() {
            Module['websocket'].emit('close', sock.stream.fd);
            sock.server = null;
          });
          sock.server.on('error', function(error) {
            // Although the ws library may pass errors that may be more descriptive than
            // ECONNREFUSED they are not necessarily the expected error code e.g.
            // ENOTFOUND on getaddrinfo seems to be node.js specific, so using EHOSTUNREACH
            // is still probably the most useful thing to do. This error shouldn't
            // occur in a well written app as errors should get trapped in the compiled
            // app's own getaddrinfo call.
            sock.error = 23; // Used in getsockopt for SOL_SOCKET/SO_ERROR test.
            Module['websocket'].emit('error', [sock.stream.fd, sock.error, 'EHOSTUNREACH: Host is unreachable']);
            // don't throw
          });
        },
  accept(listensock) {
          if (!listensock.server || !listensock.pending.length) {
            throw new FS.ErrnoError(28);
          }
          var newsock = listensock.pending.shift();
          newsock.stream.flags = listensock.stream.flags;
          return newsock;
        },
  getname(sock, peer) {
          var addr, port;
          if (peer) {
            if (sock.daddr === undefined || sock.dport === undefined) {
              throw new FS.ErrnoError(53);
            }
            addr = sock.daddr;
            port = sock.dport;
          } else {
            // TODO saddr and sport will be set for bind()'d UDP sockets, but what
            // should we be returning for TCP sockets that've been connect()'d?
            addr = sock.saddr || 0;
            port = sock.sport || 0;
          }
          return { addr, port };
        },
  sendmsg(sock, buffer, offset, length, addr, port) {
          if (sock.type === 2) {
            // connection-less sockets will honor the message address,
            // and otherwise fall back to the bound destination address
            if (addr === undefined || port === undefined) {
              addr = sock.daddr;
              port = sock.dport;
            }
            // if there was no address to fall back to, error out
            if (addr === undefined || port === undefined) {
              throw new FS.ErrnoError(17);
            }
          } else {
            // connection-based sockets will only use the bound
            addr = sock.daddr;
            port = sock.dport;
          }
  
          // find the peer for the destination address
          var dest = SOCKFS.websocket_sock_ops.getPeer(sock, addr, port);
  
          // early out if not connected with a connection-based socket
          if (sock.type === 1) {
            if (!dest || dest.socket.readyState === dest.socket.CLOSING || dest.socket.readyState === dest.socket.CLOSED) {
              throw new FS.ErrnoError(53);
            } else if (dest.socket.readyState === dest.socket.CONNECTING) {
              throw new FS.ErrnoError(6);
            }
          }
  
          // create a copy of the incoming data to send, as the WebSocket API
          // doesn't work entirely with an ArrayBufferView, it'll just send
          // the entire underlying buffer
          if (ArrayBuffer.isView(buffer)) {
            offset += buffer.byteOffset;
            buffer = buffer.buffer;
          }
  
          var data;
            data = buffer.slice(offset, offset + length);
  
          // if we're emulating a connection-less dgram socket and don't have
          // a cached connection, queue the buffer to send upon connect and
          // lie, saying the data was sent now.
          if (sock.type === 2) {
            if (!dest || dest.socket.readyState !== dest.socket.OPEN) {
              // if we're not connected, open a new connection
              if (!dest || dest.socket.readyState === dest.socket.CLOSING || dest.socket.readyState === dest.socket.CLOSED) {
                dest = SOCKFS.websocket_sock_ops.createPeer(sock, addr, port);
              }
              dest.dgram_send_queue.push(data);
              return length;
            }
          }
  
          try {
            // send the actual data
            dest.socket.send(data);
            return length;
          } catch (e) {
            throw new FS.ErrnoError(28);
          }
        },
  recvmsg(sock, length) {
          // http://pubs.opengroup.org/onlinepubs/7908799/xns/recvmsg.html
          if (sock.type === 1 && sock.server) {
            // tcp servers should not be recv()'ing on the listen socket
            throw new FS.ErrnoError(53);
          }
  
          var queued = sock.recv_queue.shift();
          if (!queued) {
            if (sock.type === 1) {
              var dest = SOCKFS.websocket_sock_ops.getPeer(sock, sock.daddr, sock.dport);
  
              if (!dest) {
                // if we have a destination address but are not connected, error out
                throw new FS.ErrnoError(53);
              }
              if (dest.socket.readyState === dest.socket.CLOSING || dest.socket.readyState === dest.socket.CLOSED) {
                // return null if the socket has closed
                return null;
              }
              // else, our socket is in a valid state but truly has nothing available
              throw new FS.ErrnoError(6);
            }
            throw new FS.ErrnoError(6);
          }
  
          // queued.data will be an ArrayBuffer if it's unadulterated, but if it's
          // requeued TCP data it'll be an ArrayBufferView
          var queuedLength = queued.data.byteLength || queued.data.length;
          var queuedOffset = queued.data.byteOffset || 0;
          var queuedBuffer = queued.data.buffer || queued.data;
          var bytesRead = Math.min(length, queuedLength);
          var res = {
            buffer: new Uint8Array(queuedBuffer, queuedOffset, bytesRead),
            addr: queued.addr,
            port: queued.port
          };
  
          // push back any unread data for TCP connections
          if (sock.type === 1 && bytesRead < queuedLength) {
            var bytesRemaining = queuedLength - bytesRead;
            queued.data = new Uint8Array(queuedBuffer, queuedOffset + bytesRead, bytesRemaining);
            sock.recv_queue.unshift(queued);
          }
  
          return res;
        },
  },
  };
  
  var getSocketFromFD = (fd) => {
      var socket = SOCKFS.getSocket(fd);
      if (!socket) throw new FS.ErrnoError(8);
      return socket;
    };
  
  var setErrNo = (value) => {
      HEAP32[((___errno_location())>>2)] = value;
      return value;
    };
  var Sockets = {
  BUFFER_SIZE:10240,
  MAX_BUFFER_SIZE:10485760,
  nextFd:1,
  fds:{
  },
  nextport:1,
  maxport:65535,
  peer:null,
  connections:{
  },
  portmap:{
  },
  localAddr:4261412874,
  addrPool:[33554442,50331658,67108874,83886090,100663306,117440522,134217738,150994954,167772170,184549386,201326602,218103818,234881034],
  };
  
  var inetPton4 = (str) => {
      var b = str.split('.');
      for (var i = 0; i < 4; i++) {
        var tmp = Number(b[i]);
        if (isNaN(tmp)) return null;
        b[i] = tmp;
      }
      return (b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)) >>> 0;
    };
  
  
  /** @suppress {checkTypes} */
  var jstoi_q = (str) => parseInt(str);
  var inetPton6 = (str) => {
      var words;
      var w, offset, z, i;
      /* http://home.deds.nl/~aeron/regex/ */
      var valid6regx = /^((?=.*::)(?!.*::.+::)(::)?([\dA-F]{1,4}:(:|\b)|){5}|([\dA-F]{1,4}:){6})((([\dA-F]{1,4}((?!\3)::|:\b|$))|(?!\2\3)){2}|(((2[0-4]|1\d|[1-9])?\d|25[0-5])\.?\b){4})$/i
      var parts = [];
      if (!valid6regx.test(str)) {
        return null;
      }
      if (str === "::") {
        return [0, 0, 0, 0, 0, 0, 0, 0];
      }
      // Z placeholder to keep track of zeros when splitting the string on ":"
      if (str.startsWith("::")) {
        str = str.replace("::", "Z:"); // leading zeros case
      } else {
        str = str.replace("::", ":Z:");
      }
  
      if (str.indexOf(".") > 0) {
        // parse IPv4 embedded stress
        str = str.replace(new RegExp('[.]', 'g'), ":");
        words = str.split(":");
        words[words.length-4] = jstoi_q(words[words.length-4]) + jstoi_q(words[words.length-3])*256;
        words[words.length-3] = jstoi_q(words[words.length-2]) + jstoi_q(words[words.length-1])*256;
        words = words.slice(0, words.length-2);
      } else {
        words = str.split(":");
      }
  
      offset = 0; z = 0;
      for (w=0; w < words.length; w++) {
        if (typeof words[w] == 'string') {
          if (words[w] === 'Z') {
            // compressed zeros - write appropriate number of zero words
            for (z = 0; z < (8 - words.length+1); z++) {
              parts[w+z] = 0;
            }
            offset = z-1;
          } else {
            // parse hex to field to 16-bit value and write it in network byte-order
            parts[w+offset] = _htons(parseInt(words[w],16));
          }
        } else {
          // parsed IPv4 words
          parts[w+offset] = words[w];
        }
      }
      return [
        (parts[1] << 16) | parts[0],
        (parts[3] << 16) | parts[2],
        (parts[5] << 16) | parts[4],
        (parts[7] << 16) | parts[6]
      ];
    };
  
  
  /** @param {number=} addrlen */
  var writeSockaddr = (sa, family, addr, port, addrlen) => {
      switch (family) {
        case 2:
          addr = inetPton4(addr);
          zeroMemory(sa, 16);
          if (addrlen) {
            HEAP32[((addrlen)>>2)] = 16;
          }
          HEAP16[((sa)>>1)] = family;
          HEAP32[(((sa)+(4))>>2)] = addr;
          HEAP16[(((sa)+(2))>>1)] = _htons(port);
          break;
        case 10:
          addr = inetPton6(addr);
          zeroMemory(sa, 28);
          if (addrlen) {
            HEAP32[((addrlen)>>2)] = 28;
          }
          HEAP32[((sa)>>2)] = family;
          HEAP32[(((sa)+(8))>>2)] = addr[0];
          HEAP32[(((sa)+(12))>>2)] = addr[1];
          HEAP32[(((sa)+(16))>>2)] = addr[2];
          HEAP32[(((sa)+(20))>>2)] = addr[3];
          HEAP16[(((sa)+(2))>>1)] = _htons(port);
          break;
        default:
          return 5;
      }
      return 0;
    };
  
  
  var DNS = {
  address_map:{
  id:1,
  addrs:{
  },
  names:{
  },
  },
  lookup_name(name) {
        // If the name is already a valid ipv4 / ipv6 address, don't generate a fake one.
        var res = inetPton4(name);
        if (res !== null) {
          return name;
        }
        res = inetPton6(name);
        if (res !== null) {
          return name;
        }
  
        // See if this name is already mapped.
        var addr;
  
        if (DNS.address_map.addrs[name]) {
          addr = DNS.address_map.addrs[name];
        } else {
          var id = DNS.address_map.id++;
          assert(id < 65535, 'exceeded max address mappings of 65535');
  
          addr = '172.29.' + (id & 0xff) + '.' + (id & 0xff00);
  
          DNS.address_map.names[addr] = name;
          DNS.address_map.addrs[name] = addr;
        }
  
        return addr;
      },
  lookup_addr(addr) {
        if (DNS.address_map.names[addr]) {
          return DNS.address_map.names[addr];
        }
  
        return null;
      },
  };
  
  function ___syscall_accept4(fd, addr, addrlen, flags, d1, d2) {
  try {
  
      var sock = getSocketFromFD(fd);
      var newsock = sock.sock_ops.accept(sock);
      if (addr) {
        var errno = writeSockaddr(addr, newsock.family, DNS.lookup_name(newsock.daddr), newsock.dport, addrlen);
        assert(!errno);
      }
      return newsock.stream.fd;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  
  var inetNtop4 = (addr) => {
      return (addr & 0xff) + '.' + ((addr >> 8) & 0xff) + '.' + ((addr >> 16) & 0xff) + '.' + ((addr >> 24) & 0xff)
    };
  
  
  var inetNtop6 = (ints) => {
      //  ref:  http://www.ietf.org/rfc/rfc2373.txt - section 2.5.4
      //  Format for IPv4 compatible and mapped  128-bit IPv6 Addresses
      //  128-bits are split into eight 16-bit words
      //  stored in network byte order (big-endian)
      //  |                80 bits               | 16 |      32 bits        |
      //  +-----------------------------------------------------------------+
      //  |               10 bytes               |  2 |      4 bytes        |
      //  +--------------------------------------+--------------------------+
      //  +               5 words                |  1 |      2 words        |
      //  +--------------------------------------+--------------------------+
      //  |0000..............................0000|0000|    IPv4 ADDRESS     | (compatible)
      //  +--------------------------------------+----+---------------------+
      //  |0000..............................0000|FFFF|    IPv4 ADDRESS     | (mapped)
      //  +--------------------------------------+----+---------------------+
      var str = "";
      var word = 0;
      var longest = 0;
      var lastzero = 0;
      var zstart = 0;
      var len = 0;
      var i = 0;
      var parts = [
        ints[0] & 0xffff,
        (ints[0] >> 16),
        ints[1] & 0xffff,
        (ints[1] >> 16),
        ints[2] & 0xffff,
        (ints[2] >> 16),
        ints[3] & 0xffff,
        (ints[3] >> 16)
      ];
  
      // Handle IPv4-compatible, IPv4-mapped, loopback and any/unspecified addresses
  
      var hasipv4 = true;
      var v4part = "";
      // check if the 10 high-order bytes are all zeros (first 5 words)
      for (i = 0; i < 5; i++) {
        if (parts[i] !== 0) { hasipv4 = false; break; }
      }
  
      if (hasipv4) {
        // low-order 32-bits store an IPv4 address (bytes 13 to 16) (last 2 words)
        v4part = inetNtop4(parts[6] | (parts[7] << 16));
        // IPv4-mapped IPv6 address if 16-bit value (bytes 11 and 12) == 0xFFFF (6th word)
        if (parts[5] === -1) {
          str = "::ffff:";
          str += v4part;
          return str;
        }
        // IPv4-compatible IPv6 address if 16-bit value (bytes 11 and 12) == 0x0000 (6th word)
        if (parts[5] === 0) {
          str = "::";
          //special case IPv6 addresses
          if (v4part === "0.0.0.0") v4part = ""; // any/unspecified address
          if (v4part === "0.0.0.1") v4part = "1";// loopback address
          str += v4part;
          return str;
        }
      }
  
      // Handle all other IPv6 addresses
  
      // first run to find the longest contiguous zero words
      for (word = 0; word < 8; word++) {
        if (parts[word] === 0) {
          if (word - lastzero > 1) {
            len = 0;
          }
          lastzero = word;
          len++;
        }
        if (len > longest) {
          longest = len;
          zstart = word - longest + 1;
        }
      }
  
      for (word = 0; word < 8; word++) {
        if (longest > 1) {
          // compress contiguous zeros - to produce "::"
          if (parts[word] === 0 && word >= zstart && word < (zstart + longest) ) {
            if (word === zstart) {
              str += ":";
              if (zstart === 0) str += ":"; //leading zeros case
            }
            continue;
          }
        }
        // converts 16-bit words from big-endian to little-endian before converting to hex string
        str += Number(_ntohs(parts[word] & 0xffff)).toString(16);
        str += word < 7 ? ":" : "";
      }
      return str;
    };
  
  var readSockaddr = (sa, salen) => {
      // family / port offsets are common to both sockaddr_in and sockaddr_in6
      var family = HEAP16[((sa)>>1)];
      var port = _ntohs(HEAPU16[(((sa)+(2))>>1)]);
      var addr;
  
      switch (family) {
        case 2:
          if (salen !== 16) {
            return { errno: 28 };
          }
          addr = HEAP32[(((sa)+(4))>>2)];
          addr = inetNtop4(addr);
          break;
        case 10:
          if (salen !== 28) {
            return { errno: 28 };
          }
          addr = [
            HEAP32[(((sa)+(8))>>2)],
            HEAP32[(((sa)+(12))>>2)],
            HEAP32[(((sa)+(16))>>2)],
            HEAP32[(((sa)+(20))>>2)]
          ];
          addr = inetNtop6(addr);
          break;
        default:
          return { errno: 5 };
      }
  
      return { family: family, addr: addr, port: port };
    };
  
  
  /** @param {boolean=} allowNull */
  var getSocketAddress = (addrp, addrlen, allowNull) => {
      if (allowNull && addrp === 0) return null;
      var info = readSockaddr(addrp, addrlen);
      if (info.errno) throw new FS.ErrnoError(info.errno);
      info.addr = DNS.lookup_addr(info.addr) || info.addr;
      return info;
    };
  
  function ___syscall_bind(fd, addr, addrlen, d1, d2, d3) {
  try {
  
      var sock = getSocketFromFD(fd);
      var info = getSocketAddress(addr, addrlen);
      sock.sock_ops.bind(sock, info.addr, info.port);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_chdir(path) {
  try {
  
      path = SYSCALLS.getStr(path);
      FS.chdir(path);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_chmod(path, mode) {
  try {
  
      path = SYSCALLS.getStr(path);
      FS.chmod(path, mode);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  
  function ___syscall_connect(fd, addr, addrlen, d1, d2, d3) {
  try {
  
      var sock = getSocketFromFD(fd);
      var info = getSocketAddress(addr, addrlen);
      sock.sock_ops.connect(sock, info.addr, info.port);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_dup(fd) {
  try {
  
      var old = SYSCALLS.getStreamFromFD(fd);
      return FS.createStream(old).fd;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_dup3(fd, newfd, flags) {
  try {
  
      var old = SYSCALLS.getStreamFromFD(fd);
      assert(!flags);
      if (old.fd === newfd) return -28;
      var existing = FS.getStream(newfd);
      if (existing) FS.close(existing);
      return FS.createStream(old, newfd).fd;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_faccessat(dirfd, path, amode, flags) {
  try {
  
      path = SYSCALLS.getStr(path);
      assert(flags === 0);
      path = SYSCALLS.calculateAt(dirfd, path);
      if (amode & ~7) {
        // need a valid mode
        return -28;
      }
      var lookup = FS.lookupPath(path, { follow: true });
      var node = lookup.node;
      if (!node) {
        return -44;
      }
      var perms = '';
      if (amode & 4) perms += 'r';
      if (amode & 2) perms += 'w';
      if (amode & 1) perms += 'x';
      if (perms /* otherwise, they've just passed F_OK */ && FS.nodePermissions(node, perms)) {
        return -2;
      }
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  var ___syscall_fadvise64 = (fd, offset, len, advice) => {
      return 0; // your advice is important to us (but we can't use it)
    };

  function ___syscall_fchdir(fd) {
  try {
  
      var stream = SYSCALLS.getStreamFromFD(fd);
      FS.chdir(stream.path);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_fchmod(fd, mode) {
  try {
  
      FS.fchmod(fd, mode);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_fchmodat(dirfd, path, mode, varargs) {
  SYSCALLS.varargs = varargs;
  try {
  
      path = SYSCALLS.getStr(path);
      path = SYSCALLS.calculateAt(dirfd, path);
      FS.chmod(path, mode);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_fchown32(fd, owner, group) {
  try {
  
      FS.fchown(fd, owner, group);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_fchownat(dirfd, path, owner, group, flags) {
  try {
  
      path = SYSCALLS.getStr(path);
      var nofollow = flags & 256;
      flags = flags & (~256);
      assert(flags === 0);
      path = SYSCALLS.calculateAt(dirfd, path);
      (nofollow ? FS.lchown : FS.chown)(path, owner, group);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  function ___syscall_fcntl64(fd, cmd, varargs) {
  SYSCALLS.varargs = varargs;
  try {
  
      var stream = SYSCALLS.getStreamFromFD(fd);
      switch (cmd) {
        case 0: {
          var arg = SYSCALLS.get();
          if (arg < 0) {
            return -28;
          }
          while (FS.streams[arg]) {
            arg++;
          }
          var newStream;
          newStream = FS.createStream(stream, arg);
          return newStream.fd;
        }
        case 1:
        case 2:
          return 0;  // FD_CLOEXEC makes no sense for a single process.
        case 3:
          return stream.flags;
        case 4: {
          var arg = SYSCALLS.get();
          stream.flags |= arg;
          return 0;
        }
        case 5: {
          var arg = SYSCALLS.getp();
          var offset = 0;
          // We're always unlocked.
          HEAP16[(((arg)+(offset))>>1)] = 2;
          return 0;
        }
        case 6:
        case 7:
          return 0; // Pretend that the locking is successful.
        case 16:
        case 8:
          return -28; // These are for sockets. We don't have them fully implemented yet.
        case 9:
          // musl trusts getown return values, due to a bug where they must be, as they overlap with errors. just return -1 here, so fcntl() returns that, and we set errno ourselves.
          setErrNo(28);
          return -1;
        default: {
          return -28;
        }
      }
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_fdatasync(fd) {
  try {
  
      var stream = SYSCALLS.getStreamFromFD(fd);
      return 0; // we can't do anything synchronously; the in-memory FS is already synced to
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_fstat64(fd, buf) {
  try {
  
      var stream = SYSCALLS.getStreamFromFD(fd);
      return SYSCALLS.doStat(FS.stat, stream.path, buf);
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_statfs64(path, size, buf) {
  try {
  
      path = SYSCALLS.getStr(path);
      assert(size === 64);
      // NOTE: None of the constants here are true. We're just returning safe and
      //       sane values.
      HEAP32[(((buf)+(4))>>2)] = 4096;
      HEAP32[(((buf)+(40))>>2)] = 4096;
      HEAP32[(((buf)+(8))>>2)] = 1000000;
      HEAP32[(((buf)+(12))>>2)] = 500000;
      HEAP32[(((buf)+(16))>>2)] = 500000;
      HEAP32[(((buf)+(20))>>2)] = FS.nextInode;
      HEAP32[(((buf)+(24))>>2)] = 1000000;
      HEAP32[(((buf)+(28))>>2)] = 42;
      HEAP32[(((buf)+(44))>>2)] = 2;  // ST_NOSUID
      HEAP32[(((buf)+(36))>>2)] = 255;
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }
  
  function ___syscall_fstatfs64(fd, size, buf) {
  try {
  
      var stream = SYSCALLS.getStreamFromFD(fd);
      return ___syscall_statfs64(0, size, buf);
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  var MAX_INT53 = 9007199254740992;
  
  var MIN_INT53 = -9007199254740992;
  var bigintToI53Checked = (num) => {
      return (num < MIN_INT53 || num > MAX_INT53) ? NaN : Number(num);
    };
  function ___syscall_ftruncate64(fd, length) {
    length = bigintToI53Checked(length);;
  
    
  try {
  
      if (isNaN(length)) return 61;
      FS.ftruncate(fd, length);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  ;
  }

  
  var stringToUTF8 = (str, outPtr, maxBytesToWrite) => {
      assert(typeof maxBytesToWrite == 'number', 'stringToUTF8(str, outPtr, maxBytesToWrite) is missing the third parameter that specifies the length of the output buffer!');
      return stringToUTF8Array(str, HEAPU8, outPtr, maxBytesToWrite);
    };
  
  function ___syscall_getcwd(buf, size) {
  try {
  
      if (size === 0) return -28;
      var cwd = FS.cwd();
      var cwdLengthInBytes = lengthBytesUTF8(cwd) + 1;
      if (size < cwdLengthInBytes) return -68;
      stringToUTF8(cwd, buf, size);
      return cwdLengthInBytes;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  function ___syscall_getdents64(fd, dirp, count) {
  try {
  
      var stream = SYSCALLS.getStreamFromFD(fd)
      if (!stream.getdents) {
        stream.getdents = FS.readdir(stream.path);
      }
  
      var struct_size = 280;
      var pos = 0;
      var off = FS.llseek(stream, 0, 1);
  
      var idx = Math.floor(off / struct_size);
  
      while (idx < stream.getdents.length && pos + struct_size <= count) {
        var id;
        var type;
        var name = stream.getdents[idx];
        if (name === '.') {
          id = stream.node.id;
          type = 4; // DT_DIR
        }
        else if (name === '..') {
          var lookup = FS.lookupPath(stream.path, { parent: true });
          id = lookup.node.id;
          type = 4; // DT_DIR
        }
        else {
          var child = FS.lookupNode(stream.node, name);
          id = child.id;
          type = FS.isChrdev(child.mode) ? 2 :  // DT_CHR, character device.
                 FS.isDir(child.mode) ? 4 :     // DT_DIR, directory.
                 FS.isLink(child.mode) ? 10 :   // DT_LNK, symbolic link.
                 8;                             // DT_REG, regular file.
        }
        assert(id);
        HEAP64[((dirp + pos)>>3)] = BigInt(id);
        HEAP64[(((dirp + pos)+(8))>>3)] = BigInt((idx + 1) * struct_size);
        HEAP16[(((dirp + pos)+(16))>>1)] = 280;
        HEAP8[(((dirp + pos)+(18))>>0)] = type;
        stringToUTF8(name, dirp + pos + 19, 256);
        pos += struct_size;
        idx += 1;
      }
      FS.llseek(stream, idx * struct_size, 0);
      return pos;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  
  
  function ___syscall_getpeername(fd, addr, addrlen, d1, d2, d3) {
  try {
  
      var sock = getSocketFromFD(fd);
      if (!sock.daddr) {
        return -53; // The socket is not connected.
      }
      var errno = writeSockaddr(addr, sock.family, DNS.lookup_name(sock.daddr), sock.dport, addrlen);
      assert(!errno);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  
  
  function ___syscall_getsockname(fd, addr, addrlen, d1, d2, d3) {
  try {
  
      var sock = getSocketFromFD(fd);
      // TODO: sock.saddr should never be undefined, see TODO in websocket_sock_ops.getname
      var errno = writeSockaddr(addr, sock.family, DNS.lookup_name(sock.saddr || '0.0.0.0'), sock.sport, addrlen);
      assert(!errno);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  function ___syscall_getsockopt(fd, level, optname, optval, optlen, d1) {
  try {
  
      var sock = getSocketFromFD(fd);
      // Minimal getsockopt aimed at resolving https://github.com/emscripten-core/emscripten/issues/2211
      // so only supports SOL_SOCKET with SO_ERROR.
      if (level === 1) {
        if (optname === 4) {
          HEAP32[((optval)>>2)] = sock.error;
          HEAP32[((optlen)>>2)] = 4;
          sock.error = null; // Clear the error (The SO_ERROR option obtains and then clears this field).
          return 0;
        }
      }
      return -50; // The option is unknown at the level indicated.
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_ioctl(fd, op, varargs) {
  SYSCALLS.varargs = varargs;
  try {
  
      var stream = SYSCALLS.getStreamFromFD(fd);
      switch (op) {
        case 21509: {
          if (!stream.tty) return -59;
          return 0;
        }
        case 21505: {
          if (!stream.tty) return -59;
          if (stream.tty.ops.ioctl_tcgets) {
            var termios = stream.tty.ops.ioctl_tcgets(stream);
            var argp = SYSCALLS.getp();
            HEAP32[((argp)>>2)] = termios.c_iflag || 0;
            HEAP32[(((argp)+(4))>>2)] = termios.c_oflag || 0;
            HEAP32[(((argp)+(8))>>2)] = termios.c_cflag || 0;
            HEAP32[(((argp)+(12))>>2)] = termios.c_lflag || 0;
            for (var i = 0; i < 32; i++) {
              HEAP8[(((argp + i)+(17))>>0)] = termios.c_cc[i] || 0;
            }
            return 0;
          }
          return 0;
        }
        case 21510:
        case 21511:
        case 21512: {
          if (!stream.tty) return -59;
          return 0; // no-op, not actually adjusting terminal settings
        }
        case 21506:
        case 21507:
        case 21508: {
          if (!stream.tty) return -59;
          if (stream.tty.ops.ioctl_tcsets) {
            var argp = SYSCALLS.getp();
            var c_iflag = HEAP32[((argp)>>2)];
            var c_oflag = HEAP32[(((argp)+(4))>>2)];
            var c_cflag = HEAP32[(((argp)+(8))>>2)];
            var c_lflag = HEAP32[(((argp)+(12))>>2)];
            var c_cc = []
            for (var i = 0; i < 32; i++) {
              c_cc.push(HEAP8[(((argp + i)+(17))>>0)]);
            }
            return stream.tty.ops.ioctl_tcsets(stream.tty, op, { c_iflag, c_oflag, c_cflag, c_lflag, c_cc });
          }
          return 0; // no-op, not actually adjusting terminal settings
        }
        case 21519: {
          if (!stream.tty) return -59;
          var argp = SYSCALLS.getp();
          HEAP32[((argp)>>2)] = 0;
          return 0;
        }
        case 21520: {
          if (!stream.tty) return -59;
          return -28; // not supported
        }
        case 21531: {
          var argp = SYSCALLS.getp();
          return FS.ioctl(stream, op, argp);
        }
        case 21523: {
          // TODO: in theory we should write to the winsize struct that gets
          // passed in, but for now musl doesn't read anything on it
          if (!stream.tty) return -59;
          if (stream.tty.ops.ioctl_tiocgwinsz) {
            var winsize = stream.tty.ops.ioctl_tiocgwinsz(stream.tty);
            var argp = SYSCALLS.getp();
            HEAP16[((argp)>>1)] = winsize[0];
            HEAP16[(((argp)+(2))>>1)] = winsize[1];
          }
          return 0;
        }
        case 21524: {
          // TODO: technically, this ioctl call should change the window size.
          // but, since emscripten doesn't have any concept of a terminal window
          // yet, we'll just silently throw it away as we do TIOCGWINSZ
          if (!stream.tty) return -59;
          return 0;
        }
        case 21515: {
          if (!stream.tty) return -59;
          return 0;
        }
        default: return -28; // not supported
      }
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  function ___syscall_listen(fd, backlog) {
  try {
  
      var sock = getSocketFromFD(fd);
      sock.sock_ops.listen(sock, backlog);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_lstat64(path, buf) {
  try {
  
      path = SYSCALLS.getStr(path);
      return SYSCALLS.doStat(FS.lstat, path, buf);
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_mkdirat(dirfd, path, mode) {
  try {
  
      path = SYSCALLS.getStr(path);
      path = SYSCALLS.calculateAt(dirfd, path);
      // remove a trailing slash, if one - /a/b/ has basename of '', but
      // we want to create b in the context of this function
      path = PATH.normalize(path);
      if (path[path.length-1] === '/') path = path.substr(0, path.length-1);
      FS.mkdir(path, mode, 0);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_newfstatat(dirfd, path, buf, flags) {
  try {
  
      path = SYSCALLS.getStr(path);
      var nofollow = flags & 256;
      var allowEmpty = flags & 4096;
      flags = flags & (~6400);
      assert(!flags, `unknown flags in __syscall_newfstatat: ${flags}`);
      path = SYSCALLS.calculateAt(dirfd, path, allowEmpty);
      return SYSCALLS.doStat(nofollow ? FS.lstat : FS.stat, path, buf);
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_openat(dirfd, path, flags, varargs) {
  SYSCALLS.varargs = varargs;
  try {
  
      path = SYSCALLS.getStr(path);
      path = SYSCALLS.calculateAt(dirfd, path);
      var mode = varargs ? SYSCALLS.get() : 0;
      return FS.open(path, flags, mode).fd;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  var PIPEFS = {
  BUCKET_BUFFER_SIZE:8192,
  mount(mount) {
        // Do not pollute the real root directory or its child nodes with pipes
        // Looks like it is OK to create another pseudo-root node not linked to the FS.root hierarchy this way
        return FS.createNode(null, '/', 16384 | 511 /* 0777 */, 0);
      },
  createPipe() {
        var pipe = {
          buckets: [],
          // refcnt 2 because pipe has a read end and a write end. We need to be
          // able to read from the read end after write end is closed.
          refcnt : 2,
        };
  
        pipe.buckets.push({
          buffer: new Uint8Array(PIPEFS.BUCKET_BUFFER_SIZE),
          offset: 0,
          roffset: 0
        });
  
        var rName = PIPEFS.nextname();
        var wName = PIPEFS.nextname();
        var rNode = FS.createNode(PIPEFS.root, rName, 4096, 0);
        var wNode = FS.createNode(PIPEFS.root, wName, 4096, 0);
  
        rNode.pipe = pipe;
        wNode.pipe = pipe;
  
        var readableStream = FS.createStream({
          path: rName,
          node: rNode,
          flags: 0,
          seekable: false,
          stream_ops: PIPEFS.stream_ops
        });
        rNode.stream = readableStream;
  
        var writableStream = FS.createStream({
          path: wName,
          node: wNode,
          flags: 1,
          seekable: false,
          stream_ops: PIPEFS.stream_ops
        });
        wNode.stream = writableStream;
  
        return {
          readable_fd: readableStream.fd,
          writable_fd: writableStream.fd
        };
      },
  stream_ops:{
  poll(stream) {
          var pipe = stream.node.pipe;
  
          if ((stream.flags & 2097155) === 1) {
            return (256 | 4);
          }
          if (pipe.buckets.length > 0) {
            for (var i = 0; i < pipe.buckets.length; i++) {
              var bucket = pipe.buckets[i];
              if (bucket.offset - bucket.roffset > 0) {
                return (64 | 1);
              }
            }
          }
  
          return 0;
        },
  ioctl(stream, request, varargs) {
          return 28;
        },
  fsync(stream) {
          return 28;
        },
  read(stream, buffer, offset, length, position /* ignored */) {
          var pipe = stream.node.pipe;
          var currentLength = 0;
  
          for (var i = 0; i < pipe.buckets.length; i++) {
            var bucket = pipe.buckets[i];
            currentLength += bucket.offset - bucket.roffset;
          }
  
          assert(buffer instanceof ArrayBuffer || ArrayBuffer.isView(buffer));
          var data = buffer.subarray(offset, offset + length);
  
          if (length <= 0) {
            return 0;
          }
          if (currentLength == 0) {
            // Behave as if the read end is always non-blocking
            throw new FS.ErrnoError(6);
          }
          var toRead = Math.min(currentLength, length);
  
          var totalRead = toRead;
          var toRemove = 0;
  
          for (var i = 0; i < pipe.buckets.length; i++) {
            var currBucket = pipe.buckets[i];
            var bucketSize = currBucket.offset - currBucket.roffset;
  
            if (toRead <= bucketSize) {
              var tmpSlice = currBucket.buffer.subarray(currBucket.roffset, currBucket.offset);
              if (toRead < bucketSize) {
                tmpSlice = tmpSlice.subarray(0, toRead);
                currBucket.roffset += toRead;
              } else {
                toRemove++;
              }
              data.set(tmpSlice);
              break;
            } else {
              var tmpSlice = currBucket.buffer.subarray(currBucket.roffset, currBucket.offset);
              data.set(tmpSlice);
              data = data.subarray(tmpSlice.byteLength);
              toRead -= tmpSlice.byteLength;
              toRemove++;
            }
          }
  
          if (toRemove && toRemove == pipe.buckets.length) {
            // Do not generate excessive garbage in use cases such as
            // write several bytes, read everything, write several bytes, read everything...
            toRemove--;
            pipe.buckets[toRemove].offset = 0;
            pipe.buckets[toRemove].roffset = 0;
          }
  
          pipe.buckets.splice(0, toRemove);
  
          return totalRead;
        },
  write(stream, buffer, offset, length, position /* ignored */) {
          var pipe = stream.node.pipe;
  
          assert(buffer instanceof ArrayBuffer || ArrayBuffer.isView(buffer));
          var data = buffer.subarray(offset, offset + length);
  
          var dataLen = data.byteLength;
          if (dataLen <= 0) {
            return 0;
          }
  
          var currBucket = null;
  
          if (pipe.buckets.length == 0) {
            currBucket = {
              buffer: new Uint8Array(PIPEFS.BUCKET_BUFFER_SIZE),
              offset: 0,
              roffset: 0
            };
            pipe.buckets.push(currBucket);
          } else {
            currBucket = pipe.buckets[pipe.buckets.length - 1];
          }
  
          assert(currBucket.offset <= PIPEFS.BUCKET_BUFFER_SIZE);
  
          var freeBytesInCurrBuffer = PIPEFS.BUCKET_BUFFER_SIZE - currBucket.offset;
          if (freeBytesInCurrBuffer >= dataLen) {
            currBucket.buffer.set(data, currBucket.offset);
            currBucket.offset += dataLen;
            return dataLen;
          } else if (freeBytesInCurrBuffer > 0) {
            currBucket.buffer.set(data.subarray(0, freeBytesInCurrBuffer), currBucket.offset);
            currBucket.offset += freeBytesInCurrBuffer;
            data = data.subarray(freeBytesInCurrBuffer, data.byteLength);
          }
  
          var numBuckets = (data.byteLength / PIPEFS.BUCKET_BUFFER_SIZE) | 0;
          var remElements = data.byteLength % PIPEFS.BUCKET_BUFFER_SIZE;
  
          for (var i = 0; i < numBuckets; i++) {
            var newBucket = {
              buffer: new Uint8Array(PIPEFS.BUCKET_BUFFER_SIZE),
              offset: PIPEFS.BUCKET_BUFFER_SIZE,
              roffset: 0
            };
            pipe.buckets.push(newBucket);
            newBucket.buffer.set(data.subarray(0, PIPEFS.BUCKET_BUFFER_SIZE));
            data = data.subarray(PIPEFS.BUCKET_BUFFER_SIZE, data.byteLength);
          }
  
          if (remElements > 0) {
            var newBucket = {
              buffer: new Uint8Array(PIPEFS.BUCKET_BUFFER_SIZE),
              offset: data.byteLength,
              roffset: 0
            };
            pipe.buckets.push(newBucket);
            newBucket.buffer.set(data);
          }
  
          return dataLen;
        },
  close(stream) {
          var pipe = stream.node.pipe;
          pipe.refcnt--;
          if (pipe.refcnt === 0) {
            pipe.buckets = null;
          }
        },
  },
  nextname() {
        if (!PIPEFS.nextname.current) {
          PIPEFS.nextname.current = 0;
        }
        return 'pipe[' + (PIPEFS.nextname.current++) + ']';
      },
  };
  
  function ___syscall_pipe(fdPtr) {
  try {
  
      if (fdPtr == 0) {
        throw new FS.ErrnoError(21);
      }
  
      var res = PIPEFS.createPipe();
  
      HEAP32[((fdPtr)>>2)] = res.readable_fd;
      HEAP32[(((fdPtr)+(4))>>2)] = res.writable_fd;
  
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_poll(fds, nfds, timeout) {
  try {
  
      var nonzero = 0;
      for (var i = 0; i < nfds; i++) {
        var pollfd = fds + 8 * i;
        var fd = HEAP32[((pollfd)>>2)];
        var events = HEAP16[(((pollfd)+(4))>>1)];
        var mask = 32;
        var stream = FS.getStream(fd);
        if (stream) {
          mask = SYSCALLS.DEFAULT_POLLMASK;
          if (stream.stream_ops.poll) {
            mask = stream.stream_ops.poll(stream, -1);
          }
        }
        mask &= events | 8 | 16;
        if (mask) nonzero++;
        HEAP16[(((pollfd)+(6))>>1)] = mask;
      }
      return nonzero;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  
  function ___syscall_readlinkat(dirfd, path, buf, bufsize) {
  try {
  
      path = SYSCALLS.getStr(path);
      path = SYSCALLS.calculateAt(dirfd, path);
      if (bufsize <= 0) return -28;
      var ret = FS.readlink(path);
  
      var len = Math.min(bufsize, lengthBytesUTF8(ret));
      var endChar = HEAP8[buf+len];
      stringToUTF8(ret, buf, bufsize+1);
      // readlink is one of the rare functions that write out a C string, but does never append a null to the output buffer(!)
      // stringToUTF8() always appends a null byte, so restore the character under the null byte after the write.
      HEAP8[buf+len] = endChar;
      return len;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  
  
  function ___syscall_recvfrom(fd, buf, len, flags, addr, addrlen) {
  try {
  
      var sock = getSocketFromFD(fd);
      var msg = sock.sock_ops.recvmsg(sock, len);
      if (!msg) return 0; // socket is closed
      if (addr) {
        var errno = writeSockaddr(addr, sock.family, DNS.lookup_name(msg.addr), msg.port, addrlen);
        assert(!errno);
      }
      HEAPU8.set(msg.buffer, buf);
      return msg.buffer.byteLength;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  
  
  function ___syscall_recvmsg(fd, message, flags, d1, d2, d3) {
  try {
  
      var sock = getSocketFromFD(fd);
      var iov = HEAPU32[(((message)+(8))>>2)];
      var num = HEAP32[(((message)+(12))>>2)];
      // get the total amount of data we can read across all arrays
      var total = 0;
      for (var i = 0; i < num; i++) {
        total += HEAP32[(((iov)+((8 * i) + 4))>>2)];
      }
      // try to read total data
      var msg = sock.sock_ops.recvmsg(sock, total);
      if (!msg) return 0; // socket is closed
  
      // TODO honor flags:
      // MSG_OOB
      // Requests out-of-band data. The significance and semantics of out-of-band data are protocol-specific.
      // MSG_PEEK
      // Peeks at the incoming message.
      // MSG_WAITALL
      // Requests that the function block until the full amount of data requested can be returned. The function may return a smaller amount of data if a signal is caught, if the connection is terminated, if MSG_PEEK was specified, or if an error is pending for the socket.
  
      // write the source address out
      var name = HEAPU32[((message)>>2)];
      if (name) {
        var errno = writeSockaddr(name, sock.family, DNS.lookup_name(msg.addr), msg.port);
        assert(!errno);
      }
      // write the buffer out to the scatter-gather arrays
      var bytesRead = 0;
      var bytesRemaining = msg.buffer.byteLength;
      for (var i = 0; bytesRemaining > 0 && i < num; i++) {
        var iovbase = HEAPU32[(((iov)+((8 * i) + 0))>>2)];
        var iovlen = HEAP32[(((iov)+((8 * i) + 4))>>2)];
        if (!iovlen) {
          continue;
        }
        var length = Math.min(iovlen, bytesRemaining);
        var buf = msg.buffer.subarray(bytesRead, bytesRead + length);
        HEAPU8.set(buf, iovbase + bytesRead);
        bytesRead += length;
        bytesRemaining -= length;
      }
  
      // TODO set msghdr.msg_flags
      // MSG_EOR
      // End of record was received (if supported by the protocol).
      // MSG_OOB
      // Out-of-band data was received.
      // MSG_TRUNC
      // Normal data was truncated.
      // MSG_CTRUNC
  
      return bytesRead;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_renameat(olddirfd, oldpath, newdirfd, newpath) {
  try {
  
      oldpath = SYSCALLS.getStr(oldpath);
      newpath = SYSCALLS.getStr(newpath);
      oldpath = SYSCALLS.calculateAt(olddirfd, oldpath);
      newpath = SYSCALLS.calculateAt(newdirfd, newpath);
      FS.rename(oldpath, newpath);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_rmdir(path) {
  try {
  
      path = SYSCALLS.getStr(path);
      FS.rmdir(path);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  
  
  function ___syscall_sendmsg(fd, message, flags, d1, d2, d3) {
  try {
  
      var sock = getSocketFromFD(fd);
      var iov = HEAPU32[(((message)+(8))>>2)];
      var num = HEAP32[(((message)+(12))>>2)];
      // read the address and port to send to
      var addr, port;
      var name = HEAPU32[((message)>>2)];
      var namelen = HEAP32[(((message)+(4))>>2)];
      if (name) {
        var info = readSockaddr(name, namelen);
        if (info.errno) return -info.errno;
        port = info.port;
        addr = DNS.lookup_addr(info.addr) || info.addr;
      }
      // concatenate scatter-gather arrays into one message buffer
      var total = 0;
      for (var i = 0; i < num; i++) {
        total += HEAP32[(((iov)+((8 * i) + 4))>>2)];
      }
      var view = new Uint8Array(total);
      var offset = 0;
      for (var i = 0; i < num; i++) {
        var iovbase = HEAPU32[(((iov)+((8 * i) + 0))>>2)];
        var iovlen = HEAP32[(((iov)+((8 * i) + 4))>>2)];
        for (var j = 0; j < iovlen; j++) {
          view[offset++] = HEAP8[(((iovbase)+(j))>>0)];
        }
      }
      // write the buffer
      return sock.sock_ops.sendmsg(sock, view, 0, total, addr, port);
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  
  function ___syscall_sendto(fd, message, length, flags, addr, addr_len) {
  try {
  
      var sock = getSocketFromFD(fd);
      var dest = getSocketAddress(addr, addr_len, true);
      if (!dest) {
        // send, no address provided
        return FS.write(sock.stream, HEAP8, message, length);
      }
      // sendto an address
      return sock.sock_ops.sendmsg(sock, HEAP8, message, length, dest.addr, dest.port);
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  function ___syscall_socket(domain, type, protocol) {
  try {
  
      var sock = SOCKFS.createSocket(domain, type, protocol);
      assert(sock.stream.fd < 64); // XXX ? select() assumes socket fd values are in 0..63
      return sock.stream.fd;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  function ___syscall_stat64(path, buf) {
  try {
  
      path = SYSCALLS.getStr(path);
      return SYSCALLS.doStat(FS.stat, path, buf);
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }


  function ___syscall_symlink(target, linkpath) {
  try {
  
      target = SYSCALLS.getStr(target);
      linkpath = SYSCALLS.getStr(linkpath);
      FS.symlink(target, linkpath);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  
  function ___syscall_truncate64(path, length) {
    length = bigintToI53Checked(length);;
  
    
  try {
  
      if (isNaN(length)) return 61;
      path = SYSCALLS.getStr(path);
      FS.truncate(path, length);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  ;
  }

  function ___syscall_unlinkat(dirfd, path, flags) {
  try {
  
      path = SYSCALLS.getStr(path);
      path = SYSCALLS.calculateAt(dirfd, path);
      if (flags === 0) {
        FS.unlink(path);
      } else if (flags === 512) {
        FS.rmdir(path);
      } else {
        abort('Invalid flags passed to unlinkat');
      }
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  var readI53FromI64 = (ptr) => {
      return HEAPU32[((ptr)>>2)] + HEAP32[(((ptr)+(4))>>2)] * 4294967296;
    };
  
  function ___syscall_utimensat(dirfd, path, times, flags) {
  try {
  
      path = SYSCALLS.getStr(path);
      assert(flags === 0);
      path = SYSCALLS.calculateAt(dirfd, path, true);
      if (!times) {
        var atime = Date.now();
        var mtime = atime;
      } else {
        var seconds = readI53FromI64(times);
        var nanoseconds = HEAP32[(((times)+(8))>>2)];
        atime = (seconds*1000) + (nanoseconds/(1000*1000));
        times += 16;
        seconds = readI53FromI64(times);
        nanoseconds = HEAP32[(((times)+(8))>>2)];
        mtime = (seconds*1000) + (nanoseconds/(1000*1000));
      }
      FS.utime(path, atime, mtime);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  }

  var nowIsMonotonic = true;;
  var __emscripten_get_now_is_monotonic = () => nowIsMonotonic;

  function __gmtime_js(time, tmPtr) {
    time = bigintToI53Checked(time);;
  
    
      var date = new Date(time * 1000);
      HEAP32[((tmPtr)>>2)] = date.getUTCSeconds();
      HEAP32[(((tmPtr)+(4))>>2)] = date.getUTCMinutes();
      HEAP32[(((tmPtr)+(8))>>2)] = date.getUTCHours();
      HEAP32[(((tmPtr)+(12))>>2)] = date.getUTCDate();
      HEAP32[(((tmPtr)+(16))>>2)] = date.getUTCMonth();
      HEAP32[(((tmPtr)+(20))>>2)] = date.getUTCFullYear()-1900;
      HEAP32[(((tmPtr)+(24))>>2)] = date.getUTCDay();
      var start = Date.UTC(date.getUTCFullYear(), 0, 1, 0, 0, 0, 0);
      var yday = ((date.getTime() - start) / (1000 * 60 * 60 * 24))|0;
      HEAP32[(((tmPtr)+(28))>>2)] = yday;
    ;
  }

  var isLeapYear = (year) => {
        return year%4 === 0 && (year%100 !== 0 || year%400 === 0);
    };
  
  var MONTH_DAYS_LEAP_CUMULATIVE = [0,31,60,91,121,152,182,213,244,274,305,335];
  
  var MONTH_DAYS_REGULAR_CUMULATIVE = [0,31,59,90,120,151,181,212,243,273,304,334];
  var ydayFromDate = (date) => {
      var leap = isLeapYear(date.getFullYear());
      var monthDaysCumulative = (leap ? MONTH_DAYS_LEAP_CUMULATIVE : MONTH_DAYS_REGULAR_CUMULATIVE);
      var yday = monthDaysCumulative[date.getMonth()] + date.getDate() - 1; // -1 since it's days since Jan 1
  
      return yday;
    };
  
  function __localtime_js(time, tmPtr) {
    time = bigintToI53Checked(time);;
  
    
      var date = new Date(time*1000);
      HEAP32[((tmPtr)>>2)] = date.getSeconds();
      HEAP32[(((tmPtr)+(4))>>2)] = date.getMinutes();
      HEAP32[(((tmPtr)+(8))>>2)] = date.getHours();
      HEAP32[(((tmPtr)+(12))>>2)] = date.getDate();
      HEAP32[(((tmPtr)+(16))>>2)] = date.getMonth();
      HEAP32[(((tmPtr)+(20))>>2)] = date.getFullYear()-1900;
      HEAP32[(((tmPtr)+(24))>>2)] = date.getDay();
  
      var yday = ydayFromDate(date)|0;
      HEAP32[(((tmPtr)+(28))>>2)] = yday;
      HEAP32[(((tmPtr)+(36))>>2)] = -(date.getTimezoneOffset() * 60);
  
      // Attention: DST is in December in South, and some regions don't have DST at all.
      var start = new Date(date.getFullYear(), 0, 1);
      var summerOffset = new Date(date.getFullYear(), 6, 1).getTimezoneOffset();
      var winterOffset = start.getTimezoneOffset();
      var dst = (summerOffset != winterOffset && date.getTimezoneOffset() == Math.min(winterOffset, summerOffset))|0;
      HEAP32[(((tmPtr)+(32))>>2)] = dst;
    ;
  }

  
  var __mktime_js = function(tmPtr) {
  
    var ret = (() => { 
      var date = new Date(HEAP32[(((tmPtr)+(20))>>2)] + 1900,
                          HEAP32[(((tmPtr)+(16))>>2)],
                          HEAP32[(((tmPtr)+(12))>>2)],
                          HEAP32[(((tmPtr)+(8))>>2)],
                          HEAP32[(((tmPtr)+(4))>>2)],
                          HEAP32[((tmPtr)>>2)],
                          0);
  
      // There's an ambiguous hour when the time goes back; the tm_isdst field is
      // used to disambiguate it.  Date() basically guesses, so we fix it up if it
      // guessed wrong, or fill in tm_isdst with the guess if it's -1.
      var dst = HEAP32[(((tmPtr)+(32))>>2)];
      var guessedOffset = date.getTimezoneOffset();
      var start = new Date(date.getFullYear(), 0, 1);
      var summerOffset = new Date(date.getFullYear(), 6, 1).getTimezoneOffset();
      var winterOffset = start.getTimezoneOffset();
      var dstOffset = Math.min(winterOffset, summerOffset); // DST is in December in South
      if (dst < 0) {
        // Attention: some regions don't have DST at all.
        HEAP32[(((tmPtr)+(32))>>2)] = Number(summerOffset != winterOffset && dstOffset == guessedOffset);
      } else if ((dst > 0) != (dstOffset == guessedOffset)) {
        var nonDstOffset = Math.max(winterOffset, summerOffset);
        var trueOffset = dst > 0 ? dstOffset : nonDstOffset;
        // Don't try setMinutes(date.getMinutes() + ...) -- it's messed up.
        date.setTime(date.getTime() + (trueOffset - guessedOffset)*60000);
      }
  
      HEAP32[(((tmPtr)+(24))>>2)] = date.getDay();
      var yday = ydayFromDate(date)|0;
      HEAP32[(((tmPtr)+(28))>>2)] = yday;
      // To match expected behavior, update fields from date
      HEAP32[((tmPtr)>>2)] = date.getSeconds();
      HEAP32[(((tmPtr)+(4))>>2)] = date.getMinutes();
      HEAP32[(((tmPtr)+(8))>>2)] = date.getHours();
      HEAP32[(((tmPtr)+(12))>>2)] = date.getDate();
      HEAP32[(((tmPtr)+(16))>>2)] = date.getMonth();
      HEAP32[(((tmPtr)+(20))>>2)] = date.getYear();
  
      return date.getTime() / 1000;
     })();
    return BigInt(ret);
  };

  
  
  
  
  
  function __mmap_js(len, prot, flags, fd, offset, allocated, addr) {
    offset = bigintToI53Checked(offset);;
  
    
  try {
  
      if (isNaN(offset)) return 61;
      var stream = SYSCALLS.getStreamFromFD(fd);
      var res = FS.mmap(stream, len, offset, prot, flags);
      var ptr = res.ptr;
      HEAP32[((allocated)>>2)] = res.allocated;
      HEAPU32[((addr)>>2)] = ptr;
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  ;
  }

  
  function __msync_js(addr, len, prot, flags, fd, offset) {
    offset = bigintToI53Checked(offset);;
  
    
  try {
  
      if (isNaN(offset)) return 61;
      SYSCALLS.doMsync(addr, SYSCALLS.getStreamFromFD(fd), len, flags, offset);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  ;
  }

  
  
  
  function __munmap_js(addr, len, prot, flags, fd, offset) {
    offset = bigintToI53Checked(offset);;
  
    
  try {
  
      if (isNaN(offset)) return 61;
      var stream = SYSCALLS.getStreamFromFD(fd);
      if (prot & 2) {
        SYSCALLS.doMsync(addr, stream, len, flags, offset);
      }
      FS.munmap(stream);
      // implicitly return 0
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return -e.errno;
  }
  ;
  }

  var timers = {
  };
  
  var handleException = (e) => {
      // Certain exception types we do not treat as errors since they are used for
      // internal control flow.
      // 1. ExitStatus, which is thrown by exit()
      // 2. "unwind", which is thrown by emscripten_unwind_to_js_event_loop() and others
      //    that wish to return to JS event loop.
      if (e instanceof ExitStatus || e == 'unwind') {
        return EXITSTATUS;
      }
      checkStackCookie();
      if (e instanceof WebAssembly.RuntimeError) {
        if (_emscripten_stack_get_current() <= 0) {
          err('Stack overflow detected.  You can try increasing -sSTACK_SIZE (currently set to 5242880)');
        }
      }
      quit_(1, e);
    };
  
  
  var _proc_exit = (code) => {
      EXITSTATUS = code;
      if (!keepRuntimeAlive()) {
        if (Module['onExit']) Module['onExit'](code);
        ABORT = true;
      }
      quit_(code, new ExitStatus(code));
    };
  /** @suppress {duplicate } */
  /** @param {boolean|number=} implicit */
  var exitJS = (status, implicit) => {
      EXITSTATUS = status;
  
      checkUnflushedContent();
  
      // if exit() was called explicitly, warn the user if the runtime isn't actually being shut down
      if (keepRuntimeAlive() && !implicit) {
        var msg = `program exited (with status: ${status}), but keepRuntimeAlive() is set (counter=${runtimeKeepaliveCounter}) due to an async operation, so halting execution but not exiting the runtime or preventing further async execution (you can use emscripten_force_exit, if you want to force a true shutdown)`;
        readyPromiseReject(msg);
        err(msg);
      }
  
      _proc_exit(status);
    };
  var _exit = exitJS;
  
  var maybeExit = () => {
      if (!keepRuntimeAlive()) {
        try {
          _exit(EXITSTATUS);
        } catch (e) {
          handleException(e);
        }
      }
    };
  var callUserCallback = (func) => {
      if (ABORT) {
        err('user callback triggered after runtime exited or application aborted.  Ignoring.');
        return;
      }
      try {
        func();
        maybeExit();
      } catch (e) {
        handleException(e);
      }
    };
  
  
  var _emscripten_get_now;
      // Modern environment where performance.now() is supported:
      // N.B. a shorter form "_emscripten_get_now = performance.now;" is
      // unfortunately not allowed even in current browsers (e.g. FF Nightly 75).
      _emscripten_get_now = () => performance.now();
  ;
  var __setitimer_js = (which, timeout_ms) => {
      // First, clear any existing timer.
      if (timers[which]) {
        clearTimeout(timers[which].id);
        delete timers[which];
      }
  
      // A timeout of zero simply cancels the current timeout so we have nothing
      // more to do.
      if (!timeout_ms) return 0;
  
      var id = setTimeout(() => {
        assert(which in timers);
        delete timers[which];
        callUserCallback(() => __emscripten_timeout(which, _emscripten_get_now()));
      }, timeout_ms);
      timers[which] = { id, timeout_ms };
      return 0;
    };

  
  
  var stringToNewUTF8 = (str) => {
      var size = lengthBytesUTF8(str) + 1;
      var ret = _malloc(size);
      if (ret) stringToUTF8(str, ret, size);
      return ret;
    };
  var __tzset_js = (timezone, daylight, tzname) => {
      // TODO: Use (malleable) environment variables instead of system settings.
      var currentYear = new Date().getFullYear();
      var winter = new Date(currentYear, 0, 1);
      var summer = new Date(currentYear, 6, 1);
      var winterOffset = winter.getTimezoneOffset();
      var summerOffset = summer.getTimezoneOffset();
  
      // Local standard timezone offset. Local standard time is not adjusted for daylight savings.
      // This code uses the fact that getTimezoneOffset returns a greater value during Standard Time versus Daylight Saving Time (DST).
      // Thus it determines the expected output during Standard Time, and it compares whether the output of the given date the same (Standard) or less (DST).
      var stdTimezoneOffset = Math.max(winterOffset, summerOffset);
  
      // timezone is specified as seconds west of UTC ("The external variable
      // `timezone` shall be set to the difference, in seconds, between
      // Coordinated Universal Time (UTC) and local standard time."), the same
      // as returned by stdTimezoneOffset.
      // See http://pubs.opengroup.org/onlinepubs/009695399/functions/tzset.html
      HEAPU32[((timezone)>>2)] = stdTimezoneOffset * 60;
  
      HEAP32[((daylight)>>2)] = Number(winterOffset != summerOffset);
  
      function extractZone(date) {
        var match = date.toTimeString().match(/\(([A-Za-z ]+)\)$/);
        return match ? match[1] : "GMT";
      };
      var winterName = extractZone(winter);
      var summerName = extractZone(summer);
      var winterNamePtr = stringToNewUTF8(winterName);
      var summerNamePtr = stringToNewUTF8(summerName);
      if (summerOffset < winterOffset) {
        // Northern hemisphere
        HEAPU32[((tzname)>>2)] = winterNamePtr;
        HEAPU32[(((tzname)+(4))>>2)] = summerNamePtr;
      } else {
        HEAPU32[((tzname)>>2)] = summerNamePtr;
        HEAPU32[(((tzname)+(4))>>2)] = winterNamePtr;
      }
    };

  var _abort = () => {
      abort('native code called abort()');
    };

  var readEmAsmArgsArray = [];
  var readEmAsmArgs = (sigPtr, buf) => {
      // Nobody should have mutated _readEmAsmArgsArray underneath us to be something else than an array.
      assert(Array.isArray(readEmAsmArgsArray));
      // The input buffer is allocated on the stack, so it must be stack-aligned.
      assert(buf % 16 == 0);
      readEmAsmArgsArray.length = 0;
      var ch;
      // Most arguments are i32s, so shift the buffer pointer so it is a plain
      // index into HEAP32.
      while (ch = HEAPU8[sigPtr++]) {
        var chr = String.fromCharCode(ch);
        var validChars = ['d', 'f', 'i'];
        // In WASM_BIGINT mode we support passing i64 values as bigint.
        validChars.push('j');
        assert(validChars.includes(chr), `Invalid character ${ch}("${chr}") in readEmAsmArgs! Use only [${validChars}], and do not specify "v" for void return argument.`);
        // Floats are always passed as doubles, so all types except for 'i'
        // are 8 bytes and require alignment.
        buf += (ch != 105) && buf % 8 ? 4 : 0;
        readEmAsmArgsArray.push(
          ch == 106 ?  HEAP64[((buf)>>3)] :
          ch == 105 ?
            HEAP32[((buf)>>2)] :
            HEAPF64[((buf)>>3)]
        );
        buf += ch == 105 ? 4 : 8;
      }
      return readEmAsmArgsArray;
    };
  var runEmAsmFunction = (code, sigPtr, argbuf) => {
      var args = readEmAsmArgs(sigPtr, argbuf);
      if (!ASM_CONSTS.hasOwnProperty(code)) abort(`No EM_ASM constant found at address ${code}`);
      return ASM_CONSTS[code].apply(null, args);
    };
  var _emscripten_asm_const_int = (code, sigPtr, argbuf) => {
      return runEmAsmFunction(code, sigPtr, argbuf);
    };

  var _emscripten_date_now = () => Date.now();

  var _emscripten_err = (str) => err(UTF8ToString(str));

  var _emscripten_exit_with_live_runtime = () => {
      
      throw 'unwind';
    };

  var getHeapMax = () =>
      HEAPU8.length;
  var _emscripten_get_heap_max = () => getHeapMax();


  var _emscripten_get_now_res = () => { // return resolution of get_now, in nanoseconds
      if (ENVIRONMENT_IS_NODE) {
        return 1; // nanoseconds
      }
      // Modern environment where performance.now() is supported:
      return 1000; // microseconds (1/1000 of a millisecond)
    };

  var _emscripten_memcpy_big = (dest, src, num) => HEAPU8.copyWithin(dest, src, src + num);

  
  var abortOnCannotGrowMemory = (requestedSize) => {
      abort(`Cannot enlarge memory arrays to size ${requestedSize} bytes (OOM). Either (1) compile with -sINITIAL_MEMORY=X with X higher than the current value ${HEAP8.length}, (2) compile with -sALLOW_MEMORY_GROWTH which allows increasing the size at runtime, or (3) if you want malloc to return NULL (0) instead of this abort, compile with -sABORTING_MALLOC=0`);
    };
  var _emscripten_resize_heap = (requestedSize) => {
      var oldSize = HEAPU8.length;
      // With CAN_ADDRESS_2GB or MEMORY64, pointers are already unsigned.
      requestedSize >>>= 0;
      abortOnCannotGrowMemory(requestedSize);
    };

  var ENV = {
  };
  
  var getExecutableName = () => {
      return thisProgram || './this.program';
    };
  var getEnvStrings = () => {
      if (!getEnvStrings.strings) {
        // Default values.
        // Browser language detection #8751
        var lang = ((typeof navigator == 'object' && navigator.languages && navigator.languages[0]) || 'C').replace('-', '_') + '.UTF-8';
        var env = {
          'USER': 'web_user',
          'LOGNAME': 'web_user',
          'PATH': '/',
          'PWD': '/',
          'HOME': '/home/web_user',
          'LANG': lang,
          '_': getExecutableName()
        };
        // Apply the user-provided values, if any.
        for (var x in ENV) {
          // x is a key in ENV; if ENV[x] is undefined, that means it was
          // explicitly set to be so. We allow user code to do that to
          // force variables with default values to remain unset.
          if (ENV[x] === undefined) delete env[x];
          else env[x] = ENV[x];
        }
        var strings = [];
        for (var x in env) {
          strings.push(`${x}=${env[x]}`);
        }
        getEnvStrings.strings = strings;
      }
      return getEnvStrings.strings;
    };
  
  var stringToAscii = (str, buffer) => {
      for (var i = 0; i < str.length; ++i) {
        assert(str.charCodeAt(i) === (str.charCodeAt(i) & 0xff));
        HEAP8[((buffer++)>>0)] = str.charCodeAt(i);
      }
      // Null-terminate the string
      HEAP8[((buffer)>>0)] = 0;
    };
  
  var _environ_get = (__environ, environ_buf) => {
      var bufSize = 0;
      getEnvStrings().forEach((string, i) => {
        var ptr = environ_buf + bufSize;
        HEAPU32[(((__environ)+(i*4))>>2)] = ptr;
        stringToAscii(string, ptr);
        bufSize += string.length + 1;
      });
      return 0;
    };

  
  var _environ_sizes_get = (penviron_count, penviron_buf_size) => {
      var strings = getEnvStrings();
      HEAPU32[((penviron_count)>>2)] = strings.length;
      var bufSize = 0;
      strings.forEach((string) => bufSize += string.length + 1);
      HEAPU32[((penviron_buf_size)>>2)] = bufSize;
      return 0;
    };


  function _fd_close(fd) {
  try {
  
      var stream = SYSCALLS.getStreamFromFD(fd);
      FS.close(stream);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return e.errno;
  }
  }

  function _fd_fdstat_get(fd, pbuf) {
  try {
  
      var rightsBase = 0;
      var rightsInheriting = 0;
      var flags = 0;
      {
        var stream = SYSCALLS.getStreamFromFD(fd);
        // All character devices are terminals (other things a Linux system would
        // assume is a character device, like the mouse, we have special APIs for).
        var type = stream.tty ? 2 :
                   FS.isDir(stream.mode) ? 3 :
                   FS.isLink(stream.mode) ? 7 :
                   4;
      }
      HEAP8[((pbuf)>>0)] = type;
      HEAP16[(((pbuf)+(2))>>1)] = flags;
      HEAP64[(((pbuf)+(8))>>3)] = BigInt(rightsBase);
      HEAP64[(((pbuf)+(16))>>3)] = BigInt(rightsInheriting);
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return e.errno;
  }
  }

  /** @param {number=} offset */
  var doReadv = (stream, iov, iovcnt, offset) => {
      var ret = 0;
      for (var i = 0; i < iovcnt; i++) {
        var ptr = HEAPU32[((iov)>>2)];
        var len = HEAPU32[(((iov)+(4))>>2)];
        iov += 8;
        var curr = FS.read(stream, HEAP8, ptr, len, offset);
        if (curr < 0) return -1;
        ret += curr;
        if (curr < len) break; // nothing more to read
        if (typeof offset !== 'undefined') {
          offset += curr;
        }
      }
      return ret;
    };
  
  
  function _fd_pread(fd, iov, iovcnt, offset, pnum) {
    offset = bigintToI53Checked(offset);;
  
    
  try {
  
      if (isNaN(offset)) return 61;
      var stream = SYSCALLS.getStreamFromFD(fd)
      var num = doReadv(stream, iov, iovcnt, offset);
      HEAPU32[((pnum)>>2)] = num;
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return e.errno;
  }
  ;
  }

  /** @param {number=} offset */
  var doWritev = (stream, iov, iovcnt, offset) => {
      var ret = 0;
      for (var i = 0; i < iovcnt; i++) {
        var ptr = HEAPU32[((iov)>>2)];
        var len = HEAPU32[(((iov)+(4))>>2)];
        iov += 8;
        var curr = FS.write(stream, HEAP8, ptr, len, offset);
        if (curr < 0) return -1;
        ret += curr;
        if (typeof offset !== 'undefined') {
          offset += curr;
        }
      }
      return ret;
    };
  
  
  function _fd_pwrite(fd, iov, iovcnt, offset, pnum) {
    offset = bigintToI53Checked(offset);;
  
    
  try {
  
      if (isNaN(offset)) return 61;
      var stream = SYSCALLS.getStreamFromFD(fd)
      var num = doWritev(stream, iov, iovcnt, offset);
      HEAPU32[((pnum)>>2)] = num;
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return e.errno;
  }
  ;
  }

  
  function _fd_read(fd, iov, iovcnt, pnum) {
  try {
  
      var stream = SYSCALLS.getStreamFromFD(fd);
      var num = doReadv(stream, iov, iovcnt);
      HEAPU32[((pnum)>>2)] = num;
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return e.errno;
  }
  }

  
  function _fd_seek(fd, offset, whence, newOffset) {
    offset = bigintToI53Checked(offset);;
  
    
  try {
  
      if (isNaN(offset)) return 61;
      var stream = SYSCALLS.getStreamFromFD(fd);
      FS.llseek(stream, offset, whence);
      HEAP64[((newOffset)>>3)] = BigInt(stream.position);
      if (stream.getdents && offset === 0 && whence === 0) stream.getdents = null; // reset readdir state
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return e.errno;
  }
  ;
  }

  function _fd_sync(fd) {
  try {
  
      var stream = SYSCALLS.getStreamFromFD(fd);
      if (stream.stream_ops && stream.stream_ops.fsync) {
        return stream.stream_ops.fsync(stream);
      }
      return 0; // we can't do anything synchronously; the in-memory FS is already synced to
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return e.errno;
  }
  }

  
  function _fd_write(fd, iov, iovcnt, pnum) {
  try {
  
      var stream = SYSCALLS.getStreamFromFD(fd);
      var num = doWritev(stream, iov, iovcnt);
      HEAPU32[((pnum)>>2)] = num;
      return 0;
    } catch (e) {
    if (typeof FS == 'undefined' || !(e.name === 'ErrnoError')) throw e;
    return e.errno;
  }
  }

  
  
  
  
  
  
  
  
  
  var _getaddrinfo = (node, service, hint, out) => {
      // Note getaddrinfo currently only returns a single addrinfo with ai_next defaulting to NULL. When NULL
      // hints are specified or ai_family set to AF_UNSPEC or ai_socktype or ai_protocol set to 0 then we
      // really should provide a linked list of suitable addrinfo values.
      var addrs = [];
      var canon = null;
      var addr = 0;
      var port = 0;
      var flags = 0;
      var family = 0;
      var type = 0;
      var proto = 0;
      var ai, last;
  
      function allocaddrinfo(family, type, proto, canon, addr, port) {
        var sa, salen, ai;
        var errno;
  
        salen = family === 10 ?
          28 :
          16;
        addr = family === 10 ?
          inetNtop6(addr) :
          inetNtop4(addr);
        sa = _malloc(salen);
        errno = writeSockaddr(sa, family, addr, port);
        assert(!errno);
  
        ai = _malloc(32);
        HEAP32[(((ai)+(4))>>2)] = family;
        HEAP32[(((ai)+(8))>>2)] = type;
        HEAP32[(((ai)+(12))>>2)] = proto;
        HEAPU32[(((ai)+(24))>>2)] = canon;
        HEAPU32[(((ai)+(20))>>2)] = sa;
        if (family === 10) {
          HEAP32[(((ai)+(16))>>2)] = 28;
        } else {
          HEAP32[(((ai)+(16))>>2)] = 16;
        }
        HEAP32[(((ai)+(28))>>2)] = 0;
  
        return ai;
      }
  
      if (hint) {
        flags = HEAP32[((hint)>>2)];
        family = HEAP32[(((hint)+(4))>>2)];
        type = HEAP32[(((hint)+(8))>>2)];
        proto = HEAP32[(((hint)+(12))>>2)];
      }
      if (type && !proto) {
        proto = type === 2 ? 17 : 6;
      }
      if (!type && proto) {
        type = proto === 17 ? 2 : 1;
      }
  
      // If type or proto are set to zero in hints we should really be returning multiple addrinfo values, but for
      // now default to a TCP STREAM socket so we can at least return a sensible addrinfo given NULL hints.
      if (proto === 0) {
        proto = 6;
      }
      if (type === 0) {
        type = 1;
      }
  
      if (!node && !service) {
        return -2;
      }
      if (flags & ~(1|2|4|
          1024|8|16|32)) {
        return -1;
      }
      if (hint !== 0 && (HEAP32[((hint)>>2)] & 2) && !node) {
        return -1;
      }
      if (flags & 32) {
        // TODO
        return -2;
      }
      if (type !== 0 && type !== 1 && type !== 2) {
        return -7;
      }
      if (family !== 0 && family !== 2 && family !== 10) {
        return -6;
      }
  
      if (service) {
        service = UTF8ToString(service);
        port = parseInt(service, 10);
  
        if (isNaN(port)) {
          if (flags & 1024) {
            return -2;
          }
          // TODO support resolving well-known service names from:
          // http://www.iana.org/assignments/service-names-port-numbers/service-names-port-numbers.txt
          return -8;
        }
      }
  
      if (!node) {
        if (family === 0) {
          family = 2;
        }
        if ((flags & 1) === 0) {
          if (family === 2) {
            addr = _htonl(2130706433);
          } else {
            addr = [0, 0, 0, 1];
          }
        }
        ai = allocaddrinfo(family, type, proto, null, addr, port);
        HEAPU32[((out)>>2)] = ai;
        return 0;
      }
  
      //
      // try as a numeric address
      //
      node = UTF8ToString(node);
      addr = inetPton4(node);
      if (addr !== null) {
        // incoming node is a valid ipv4 address
        if (family === 0 || family === 2) {
          family = 2;
        }
        else if (family === 10 && (flags & 8)) {
          addr = [0, 0, _htonl(0xffff), addr];
          family = 10;
        } else {
          return -2;
        }
      } else {
        addr = inetPton6(node);
        if (addr !== null) {
          // incoming node is a valid ipv6 address
          if (family === 0 || family === 10) {
            family = 10;
          } else {
            return -2;
          }
        }
      }
      if (addr != null) {
        ai = allocaddrinfo(family, type, proto, node, addr, port);
        HEAPU32[((out)>>2)] = ai;
        return 0;
      }
      if (flags & 4) {
        return -2;
      }
  
      //
      // try as a hostname
      //
      // resolve the hostname to a temporary fake address
      node = DNS.lookup_name(node);
      addr = inetPton4(node);
      if (family === 0) {
        family = 2;
      } else if (family === 10) {
        addr = [0, 0, _htonl(0xffff), addr];
      }
      ai = allocaddrinfo(family, type, proto, null, addr, port);
      HEAPU32[((out)>>2)] = ai;
      return 0;
    };

  var _getentropy = (buffer, size) => {
      randomFill(HEAPU8.subarray(buffer, buffer + size));
      return 0;
    };

  
  
  
  
  var getHostByName = (name) => {
      // generate hostent
      var ret = _malloc(20); // XXX possibly leaked, as are others here
      var nameBuf = stringToNewUTF8(name);
      HEAPU32[((ret)>>2)] = nameBuf;
      var aliasesBuf = _malloc(4);
      HEAPU32[((aliasesBuf)>>2)] = 0;
      HEAPU32[(((ret)+(4))>>2)] = aliasesBuf;
      var afinet = 2;
      HEAP32[(((ret)+(8))>>2)] = afinet;
      HEAP32[(((ret)+(12))>>2)] = 4;
      var addrListBuf = _malloc(12);
      HEAPU32[((addrListBuf)>>2)] = addrListBuf+8;
      HEAPU32[(((addrListBuf)+(4))>>2)] = 0;
      HEAP32[(((addrListBuf)+(8))>>2)] = inetPton4(DNS.lookup_name(name));
      HEAPU32[(((ret)+(16))>>2)] = addrListBuf;
      return ret;
    };
  
  
  var _gethostbyaddr = (addr, addrlen, type) => {
      if (type !== 2) {
        setErrNo(5);
        // TODO: set h_errno
        return null;
      }
      addr = HEAP32[((addr)>>2)]; // addr is in_addr
      var host = inetNtop4(addr);
      var lookup = DNS.lookup_addr(host);
      if (lookup) {
        host = lookup;
      }
      return getHostByName(host);
    };

  
  var _gethostbyname = (name) => {
      return getHostByName(UTF8ToString(name));
    };

  
  
  
  var _getnameinfo = (sa, salen, node, nodelen, serv, servlen, flags) => {
      var info = readSockaddr(sa, salen);
      if (info.errno) {
        return -6;
      }
      var port = info.port;
      var addr = info.addr;
  
      var overflowed = false;
  
      if (node && nodelen) {
        var lookup;
        if ((flags & 1) || !(lookup = DNS.lookup_addr(addr))) {
          if (flags & 8) {
            return -2;
          }
        } else {
          addr = lookup;
        }
        var numBytesWrittenExclNull = stringToUTF8(addr, node, nodelen);
  
        if (numBytesWrittenExclNull+1 >= nodelen) {
          overflowed = true;
        }
      }
  
      if (serv && servlen) {
        port = '' + port;
        var numBytesWrittenExclNull = stringToUTF8(port, serv, servlen);
  
        if (numBytesWrittenExclNull+1 >= servlen) {
          overflowed = true;
        }
      }
  
      if (overflowed) {
        // Note: even when we overflow, getnameinfo() is specced to write out the truncated results.
        return -12;
      }
  
      return 0;
    };

  var Protocols = {
  list:[],
  map:{
  },
  };
  
  
  var _setprotoent = (stayopen) => {
      // void setprotoent(int stayopen);
  
      // Allocate and populate a protoent structure given a name, protocol number and array of aliases
      function allocprotoent(name, proto, aliases) {
        // write name into buffer
        var nameBuf = _malloc(name.length + 1);
        stringToAscii(name, nameBuf);
  
        // write aliases into buffer
        var j = 0;
        var length = aliases.length;
        var aliasListBuf = _malloc((length + 1) * 4); // Use length + 1 so we have space for the terminating NULL ptr.
  
        for (var i = 0; i < length; i++, j += 4) {
          var alias = aliases[i];
          var aliasBuf = _malloc(alias.length + 1);
          stringToAscii(alias, aliasBuf);
          HEAPU32[(((aliasListBuf)+(j))>>2)] = aliasBuf;
        }
        HEAPU32[(((aliasListBuf)+(j))>>2)] = 0; // Terminating NULL pointer.
  
        // generate protoent
        var pe = _malloc(12);
        HEAPU32[((pe)>>2)] = nameBuf;
        HEAPU32[(((pe)+(4))>>2)] = aliasListBuf;
        HEAP32[(((pe)+(8))>>2)] = proto;
        return pe;
      };
  
      // Populate the protocol 'database'. The entries are limited to tcp and udp, though it is fairly trivial
      // to add extra entries from /etc/protocols if desired - though not sure if that'd actually be useful.
      var list = Protocols.list;
      var map  = Protocols.map;
      if (list.length === 0) {
          var entry = allocprotoent('tcp', 6, ['TCP']);
          list.push(entry);
          map['tcp'] = map['6'] = entry;
          entry = allocprotoent('udp', 17, ['UDP']);
          list.push(entry);
          map['udp'] = map['17'] = entry;
      }
  
      _setprotoent.index = 0;
    };
  
  
  var _getprotobyname = (name) => {
      // struct protoent *getprotobyname(const char *);
      name = UTF8ToString(name);
      _setprotoent(true);
      var result = Protocols.map[name];
      return result;
    };


  
  var arraySum = (array, index) => {
      var sum = 0;
      for (var i = 0; i <= index; sum += array[i++]) {
        // no-op
      }
      return sum;
    };
  
  
  var MONTH_DAYS_LEAP = [31,29,31,30,31,30,31,31,30,31,30,31];
  
  var MONTH_DAYS_REGULAR = [31,28,31,30,31,30,31,31,30,31,30,31];
  var addDays = (date, days) => {
      var newDate = new Date(date.getTime());
      while (days > 0) {
        var leap = isLeapYear(newDate.getFullYear());
        var currentMonth = newDate.getMonth();
        var daysInCurrentMonth = (leap ? MONTH_DAYS_LEAP : MONTH_DAYS_REGULAR)[currentMonth];
  
        if (days > daysInCurrentMonth-newDate.getDate()) {
          // we spill over to next month
          days -= (daysInCurrentMonth-newDate.getDate()+1);
          newDate.setDate(1);
          if (currentMonth < 11) {
            newDate.setMonth(currentMonth+1)
          } else {
            newDate.setMonth(0);
            newDate.setFullYear(newDate.getFullYear()+1);
          }
        } else {
          // we stay in current month
          newDate.setDate(newDate.getDate()+days);
          return newDate;
        }
      }
  
      return newDate;
    };
  
  
  
  
  var writeArrayToMemory = (array, buffer) => {
      assert(array.length >= 0, 'writeArrayToMemory array must have a length (should be an array or typed array)')
      HEAP8.set(array, buffer);
    };
  
  var _strftime = (s, maxsize, format, tm) => {
      // size_t strftime(char *restrict s, size_t maxsize, const char *restrict format, const struct tm *restrict timeptr);
      // http://pubs.opengroup.org/onlinepubs/009695399/functions/strftime.html
  
      var tm_zone = HEAPU32[(((tm)+(40))>>2)];
  
      var date = {
        tm_sec: HEAP32[((tm)>>2)],
        tm_min: HEAP32[(((tm)+(4))>>2)],
        tm_hour: HEAP32[(((tm)+(8))>>2)],
        tm_mday: HEAP32[(((tm)+(12))>>2)],
        tm_mon: HEAP32[(((tm)+(16))>>2)],
        tm_year: HEAP32[(((tm)+(20))>>2)],
        tm_wday: HEAP32[(((tm)+(24))>>2)],
        tm_yday: HEAP32[(((tm)+(28))>>2)],
        tm_isdst: HEAP32[(((tm)+(32))>>2)],
        tm_gmtoff: HEAP32[(((tm)+(36))>>2)],
        tm_zone: tm_zone ? UTF8ToString(tm_zone) : ''
      };
  
      var pattern = UTF8ToString(format);
  
      // expand format
      var EXPANSION_RULES_1 = {
        '%c': '%a %b %d %H:%M:%S %Y',     // Replaced by the locale's appropriate date and time representation - e.g., Mon Aug  3 14:02:01 2013
        '%D': '%m/%d/%y',                 // Equivalent to %m / %d / %y
        '%F': '%Y-%m-%d',                 // Equivalent to %Y - %m - %d
        '%h': '%b',                       // Equivalent to %b
        '%r': '%I:%M:%S %p',              // Replaced by the time in a.m. and p.m. notation
        '%R': '%H:%M',                    // Replaced by the time in 24-hour notation
        '%T': '%H:%M:%S',                 // Replaced by the time
        '%x': '%m/%d/%y',                 // Replaced by the locale's appropriate date representation
        '%X': '%H:%M:%S',                 // Replaced by the locale's appropriate time representation
        // Modified Conversion Specifiers
        '%Ec': '%c',                      // Replaced by the locale's alternative appropriate date and time representation.
        '%EC': '%C',                      // Replaced by the name of the base year (period) in the locale's alternative representation.
        '%Ex': '%m/%d/%y',                // Replaced by the locale's alternative date representation.
        '%EX': '%H:%M:%S',                // Replaced by the locale's alternative time representation.
        '%Ey': '%y',                      // Replaced by the offset from %EC (year only) in the locale's alternative representation.
        '%EY': '%Y',                      // Replaced by the full alternative year representation.
        '%Od': '%d',                      // Replaced by the day of the month, using the locale's alternative numeric symbols, filled as needed with leading zeros if there is any alternative symbol for zero; otherwise, with leading <space> characters.
        '%Oe': '%e',                      // Replaced by the day of the month, using the locale's alternative numeric symbols, filled as needed with leading <space> characters.
        '%OH': '%H',                      // Replaced by the hour (24-hour clock) using the locale's alternative numeric symbols.
        '%OI': '%I',                      // Replaced by the hour (12-hour clock) using the locale's alternative numeric symbols.
        '%Om': '%m',                      // Replaced by the month using the locale's alternative numeric symbols.
        '%OM': '%M',                      // Replaced by the minutes using the locale's alternative numeric symbols.
        '%OS': '%S',                      // Replaced by the seconds using the locale's alternative numeric symbols.
        '%Ou': '%u',                      // Replaced by the weekday as a number in the locale's alternative representation (Monday=1).
        '%OU': '%U',                      // Replaced by the week number of the year (Sunday as the first day of the week, rules corresponding to %U ) using the locale's alternative numeric symbols.
        '%OV': '%V',                      // Replaced by the week number of the year (Monday as the first day of the week, rules corresponding to %V ) using the locale's alternative numeric symbols.
        '%Ow': '%w',                      // Replaced by the number of the weekday (Sunday=0) using the locale's alternative numeric symbols.
        '%OW': '%W',                      // Replaced by the week number of the year (Monday as the first day of the week) using the locale's alternative numeric symbols.
        '%Oy': '%y',                      // Replaced by the year (offset from %C ) using the locale's alternative numeric symbols.
      };
      for (var rule in EXPANSION_RULES_1) {
        pattern = pattern.replace(new RegExp(rule, 'g'), EXPANSION_RULES_1[rule]);
      }
  
      var WEEKDAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
      var MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
  
      function leadingSomething(value, digits, character) {
        var str = typeof value == 'number' ? value.toString() : (value || '');
        while (str.length < digits) {
          str = character[0]+str;
        }
        return str;
      }
  
      function leadingNulls(value, digits) {
        return leadingSomething(value, digits, '0');
      }
  
      function compareByDay(date1, date2) {
        function sgn(value) {
          return value < 0 ? -1 : (value > 0 ? 1 : 0);
        }
  
        var compare;
        if ((compare = sgn(date1.getFullYear()-date2.getFullYear())) === 0) {
          if ((compare = sgn(date1.getMonth()-date2.getMonth())) === 0) {
            compare = sgn(date1.getDate()-date2.getDate());
          }
        }
        return compare;
      }
  
      function getFirstWeekStartDate(janFourth) {
          switch (janFourth.getDay()) {
            case 0: // Sunday
              return new Date(janFourth.getFullYear()-1, 11, 29);
            case 1: // Monday
              return janFourth;
            case 2: // Tuesday
              return new Date(janFourth.getFullYear(), 0, 3);
            case 3: // Wednesday
              return new Date(janFourth.getFullYear(), 0, 2);
            case 4: // Thursday
              return new Date(janFourth.getFullYear(), 0, 1);
            case 5: // Friday
              return new Date(janFourth.getFullYear()-1, 11, 31);
            case 6: // Saturday
              return new Date(janFourth.getFullYear()-1, 11, 30);
          }
      }
  
      function getWeekBasedYear(date) {
          var thisDate = addDays(new Date(date.tm_year+1900, 0, 1), date.tm_yday);
  
          var janFourthThisYear = new Date(thisDate.getFullYear(), 0, 4);
          var janFourthNextYear = new Date(thisDate.getFullYear()+1, 0, 4);
  
          var firstWeekStartThisYear = getFirstWeekStartDate(janFourthThisYear);
          var firstWeekStartNextYear = getFirstWeekStartDate(janFourthNextYear);
  
          if (compareByDay(firstWeekStartThisYear, thisDate) <= 0) {
            // this date is after the start of the first week of this year
            if (compareByDay(firstWeekStartNextYear, thisDate) <= 0) {
              return thisDate.getFullYear()+1;
            }
            return thisDate.getFullYear();
          }
          return thisDate.getFullYear()-1;
      }
  
      var EXPANSION_RULES_2 = {
        '%a': (date) => WEEKDAYS[date.tm_wday].substring(0,3) ,
        '%A': (date) => WEEKDAYS[date.tm_wday],
        '%b': (date) => MONTHS[date.tm_mon].substring(0,3),
        '%B': (date) => MONTHS[date.tm_mon],
        '%C': (date) => {
          var year = date.tm_year+1900;
          return leadingNulls((year/100)|0,2);
        },
        '%d': (date) => leadingNulls(date.tm_mday, 2),
        '%e': (date) => leadingSomething(date.tm_mday, 2, ' '),
        '%g': (date) => {
          // %g, %G, and %V give values according to the ISO 8601:2000 standard week-based year.
          // In this system, weeks begin on a Monday and week 1 of the year is the week that includes
          // January 4th, which is also the week that includes the first Thursday of the year, and
          // is also the first week that contains at least four days in the year.
          // If the first Monday of January is the 2nd, 3rd, or 4th, the preceding days are part of
          // the last week of the preceding year; thus, for Saturday 2nd January 1999,
          // %G is replaced by 1998 and %V is replaced by 53. If December 29th, 30th,
          // or 31st is a Monday, it and any following days are part of week 1 of the following year.
          // Thus, for Tuesday 30th December 1997, %G is replaced by 1998 and %V is replaced by 01.
  
          return getWeekBasedYear(date).toString().substring(2);
        },
        '%G': (date) => getWeekBasedYear(date),
        '%H': (date) => leadingNulls(date.tm_hour, 2),
        '%I': (date) => {
          var twelveHour = date.tm_hour;
          if (twelveHour == 0) twelveHour = 12;
          else if (twelveHour > 12) twelveHour -= 12;
          return leadingNulls(twelveHour, 2);
        },
        '%j': (date) => {
          // Day of the year (001-366)
          return leadingNulls(date.tm_mday + arraySum(isLeapYear(date.tm_year+1900) ? MONTH_DAYS_LEAP : MONTH_DAYS_REGULAR, date.tm_mon-1), 3);
        },
        '%m': (date) => leadingNulls(date.tm_mon+1, 2),
        '%M': (date) => leadingNulls(date.tm_min, 2),
        '%n': () => '\n',
        '%p': (date) => {
          if (date.tm_hour >= 0 && date.tm_hour < 12) {
            return 'AM';
          }
          return 'PM';
        },
        '%S': (date) => leadingNulls(date.tm_sec, 2),
        '%t': () => '\t',
        '%u': (date) => date.tm_wday || 7,
        '%U': (date) => {
          var days = date.tm_yday + 7 - date.tm_wday;
          return leadingNulls(Math.floor(days / 7), 2);
        },
        '%V': (date) => {
          // Replaced by the week number of the year (Monday as the first day of the week)
          // as a decimal number [01,53]. If the week containing 1 January has four
          // or more days in the new year, then it is considered week 1.
          // Otherwise, it is the last week of the previous year, and the next week is week 1.
          // Both January 4th and the first Thursday of January are always in week 1. [ tm_year, tm_wday, tm_yday]
          var val = Math.floor((date.tm_yday + 7 - (date.tm_wday + 6) % 7 ) / 7);
          // If 1 Jan is just 1-3 days past Monday, the previous week
          // is also in this year.
          if ((date.tm_wday + 371 - date.tm_yday - 2) % 7 <= 2) {
            val++;
          }
          if (!val) {
            val = 52;
            // If 31 December of prev year a Thursday, or Friday of a
            // leap year, then the prev year has 53 weeks.
            var dec31 = (date.tm_wday + 7 - date.tm_yday - 1) % 7;
            if (dec31 == 4 || (dec31 == 5 && isLeapYear(date.tm_year%400-1))) {
              val++;
            }
          } else if (val == 53) {
            // If 1 January is not a Thursday, and not a Wednesday of a
            // leap year, then this year has only 52 weeks.
            var jan1 = (date.tm_wday + 371 - date.tm_yday) % 7;
            if (jan1 != 4 && (jan1 != 3 || !isLeapYear(date.tm_year)))
              val = 1;
          }
          return leadingNulls(val, 2);
        },
        '%w': (date) => date.tm_wday,
        '%W': (date) => {
          var days = date.tm_yday + 7 - ((date.tm_wday + 6) % 7);
          return leadingNulls(Math.floor(days / 7), 2);
        },
        '%y': (date) => {
          // Replaced by the last two digits of the year as a decimal number [00,99]. [ tm_year]
          return (date.tm_year+1900).toString().substring(2);
        },
        // Replaced by the year as a decimal number (for example, 1997). [ tm_year]
        '%Y': (date) => date.tm_year+1900,
        '%z': (date) => {
          // Replaced by the offset from UTC in the ISO 8601:2000 standard format ( +hhmm or -hhmm ).
          // For example, "-0430" means 4 hours 30 minutes behind UTC (west of Greenwich).
          var off = date.tm_gmtoff;
          var ahead = off >= 0;
          off = Math.abs(off) / 60;
          // convert from minutes into hhmm format (which means 60 minutes = 100 units)
          off = (off / 60)*100 + (off % 60);
          return (ahead ? '+' : '-') + String("0000" + off).slice(-4);
        },
        '%Z': (date) => date.tm_zone,
        '%%': () => '%'
      };
  
      // Replace %% with a pair of NULLs (which cannot occur in a C string), then
      // re-inject them after processing.
      pattern = pattern.replace(/%%/g, '\0\0')
      for (var rule in EXPANSION_RULES_2) {
        if (pattern.includes(rule)) {
          pattern = pattern.replace(new RegExp(rule, 'g'), EXPANSION_RULES_2[rule](date));
        }
      }
      pattern = pattern.replace(/\0\0/g, '%')
  
      var bytes = intArrayFromString(pattern, false);
      if (bytes.length > maxsize) {
        return 0;
      }
  
      writeArrayToMemory(bytes, s);
      return bytes.length-1;
    };

  
  var _system = (command) => {
      if (ENVIRONMENT_IS_NODE) {
        if (!command) return 1; // shell is available
  
        var cmdstr = UTF8ToString(command);
        if (!cmdstr.length) return 0; // this is what glibc seems to do (shell works test?)
  
        var cp = require('child_process');
        var ret = cp.spawnSync(cmdstr, [], {shell:true, stdio:'inherit'});
  
        var _W_EXITCODE = (ret, sig) => ((ret) << 8 | (sig));
  
        // this really only can happen if process is killed by signal
        if (ret.status === null) {
          // sadly node doesn't expose such function
          var signalToNumber = (sig) => {
            // implement only the most common ones, and fallback to SIGINT
            switch (sig) {
              case 'SIGHUP': return 1;
              case 'SIGINT': return 2;
              case 'SIGQUIT': return 3;
              case 'SIGFPE': return 8;
              case 'SIGKILL': return 9;
              case 'SIGALRM': return 14;
              case 'SIGTERM': return 15;
            }
            return 2; // SIGINT
          }
          return _W_EXITCODE(0, signalToNumber(ret.signal));
        }
  
        return _W_EXITCODE(ret.status, 0);
      }
      // int system(const char *command);
      // http://pubs.opengroup.org/onlinepubs/000095399/functions/system.html
      // Can't call external programs.
      if (!command) return 0; // no shell available
      setErrNo(52);
      return -1;
    };



  
  var stringToUTF8OnStack = (str) => {
      var size = lengthBytesUTF8(str) + 1;
      var ret = stackAlloc(size);
      stringToUTF8(str, ret, size);
      return ret;
    };


  var setWasmTableEntry = (idx, func) => {
      wasmTable.set(idx, func);
      // With ABORT_ON_WASM_EXCEPTIONS wasmTable.get is overriden to return wrapped
      // functions so we need to call it here to retrieve the potential wrapper correctly
      // instead of just storing 'func' directly into wasmTableMirror
      wasmTableMirror[idx] = wasmTable.get(idx);
    };

  var freeTableIndexes = [];
  var getEmptyTableSlot = () => {
      // Reuse a free index if there is one, otherwise grow.
      if (freeTableIndexes.length) {
        return freeTableIndexes.pop();
      }
      // Grow the table
      try {
        wasmTable.grow(1);
      } catch (err) {
        if (!(err instanceof RangeError)) {
          throw err;
        }
        throw 'Unable to grow wasm table. Set ALLOW_TABLE_GROWTH.';
      }
      return wasmTable.length - 1;
    };

  var uleb128Encode = (n, target) => {
      assert(n < 16384);
      if (n < 128) {
        target.push(n);
      } else {
        target.push((n % 128) | 128, n >> 7);
      }
    };
  
  var sigToWasmTypes = (sig) => {
      var typeNames = {
        'i': 'i32',
        'j': 'i64',
        'f': 'f32',
        'd': 'f64',
        'e': 'externref',
        'p': 'i32',
      };
      var type = {
        parameters: [],
        results: sig[0] == 'v' ? [] : [typeNames[sig[0]]]
      };
      for (var i = 1; i < sig.length; ++i) {
        assert(sig[i] in typeNames, 'invalid signature char: ' + sig[i]);
        type.parameters.push(typeNames[sig[i]]);
      }
      return type;
    };
  
  var generateFuncType = (sig, target) => {
      var sigRet = sig.slice(0, 1);
      var sigParam = sig.slice(1);
      var typeCodes = {
        'i': 0x7f, // i32
        'p': 0x7f, // i32
        'j': 0x7e, // i64
        'f': 0x7d, // f32
        'd': 0x7c, // f64
      };
  
      // Parameters, length + signatures
      target.push(0x60 /* form: func */);
      uleb128Encode(sigParam.length, target);
      for (var i = 0; i < sigParam.length; ++i) {
        assert(sigParam[i] in typeCodes, 'invalid signature char: ' + sigParam[i]);
    target.push(typeCodes[sigParam[i]]);
      }
    
      // Return values, length + signatures
      // With no multi-return in MVP, either 0 (void) or 1 (anything else)
      if (sigRet == 'v') {
        target.push(0x00);
      } else {
        target.push(0x01, typeCodes[sigRet]);
      }
    };
  var convertJsFunctionToWasm = (func, sig) => {
  
      // If the type reflection proposal is available, use the new
      // "WebAssembly.Function" constructor.
      // Otherwise, construct a minimal wasm module importing the JS function and
      // re-exporting it.
      if (typeof WebAssembly.Function == "function") {
        return new WebAssembly.Function(sigToWasmTypes(sig), func);
      }
  
      // The module is static, with the exception of the type section, which is
      // generated based on the signature passed in.
      var typeSectionBody = [
        0x01, // count: 1
      ];
      generateFuncType(sig, typeSectionBody);
  
      // Rest of the module is static
      var bytes = [
        0x00, 0x61, 0x73, 0x6d, // magic ("\0asm")
        0x01, 0x00, 0x00, 0x00, // version: 1
        0x01, // Type section code
      ];
      // Write the overall length of the type section followed by the body
      uleb128Encode(typeSectionBody.length, bytes);
      bytes.push.apply(bytes, typeSectionBody);
  
      // The rest of the module is static
      bytes.push(
        0x02, 0x07, // import section
          // (import "e" "f" (func 0 (type 0)))
          0x01, 0x01, 0x65, 0x01, 0x66, 0x00, 0x00,
        0x07, 0x05, // export section
          // (export "f" (func 0 (type 0)))
          0x01, 0x01, 0x66, 0x00, 0x00,
      );
  
      // We can compile this wasm module synchronously because it is very small.
      // This accepts an import (at "e.f"), that it reroutes to an export (at "f")
      var module = new WebAssembly.Module(new Uint8Array(bytes));
      var instance = new WebAssembly.Instance(module, { 'e': { 'f': func } });
      var wrappedFunc = instance.exports['f'];
      return wrappedFunc;
    };






  var STACK_SIZE = 5242880;




  var growMemory = (size) => {
      var b = wasmMemory.buffer;
      var pages = (size - b.byteLength + 65535) / 65536;
      try {
        // round size grow request up to wasm page size (fixed 64KB per spec)
        wasmMemory.grow(pages); // .grow() takes a delta compared to the previous size
        updateMemoryViews();
        return 1 /*success*/;
      } catch(e) {
        err(`growMemory: Attempted to grow heap from ${b.byteLength} bytes to ${size} bytes, but got error: ${e}`);
      }
      // implicit 0 return to save code size (caller will cast "undefined" into 0
      // anyhow)
    };


      var waitBuffer;
      try {
        var SharedArrayBuffer = new WebAssembly.Memory({"shared":true,"initial":0,"maximum":0}).buffer.constructor;
        waitBuffer = new Int32Array(new SharedArrayBuffer(4));
      } catch(_) { }
    ;

  var FSNode = /** @constructor */ function(parent, name, mode, rdev) {
    if (!parent) {
      parent = this;  // root node sets parent to itself
    }
    this.parent = parent;
    this.mount = parent.mount;
    this.mounted = null;
    this.id = FS.nextInode++;
    this.name = name;
    this.mode = mode;
    this.node_ops = {};
    this.stream_ops = {};
    this.rdev = rdev;
  };
  var readMode = 292/*292*/ | 73/*73*/;
  var writeMode = 146/*146*/;
  Object.defineProperties(FSNode.prototype, {
   read: {
    get: /** @this{FSNode} */function() {
     return (this.mode & readMode) === readMode;
    },
    set: /** @this{FSNode} */function(val) {
     val ? this.mode |= readMode : this.mode &= ~readMode;
    }
   },
   write: {
    get: /** @this{FSNode} */function() {
     return (this.mode & writeMode) === writeMode;
    },
    set: /** @this{FSNode} */function(val) {
     val ? this.mode |= writeMode : this.mode &= ~writeMode;
    }
   },
   isFolder: {
    get: /** @this{FSNode} */function() {
     return FS.isDir(this.mode);
    }
   },
   isDevice: {
    get: /** @this{FSNode} */function() {
     return FS.isChrdev(this.mode);
    }
   }
  });
  FS.FSNode = FSNode;
  FS.createPreloadedFile = FS_createPreloadedFile;
  FS.staticInit();;
if (ENVIRONMENT_IS_NODE) { NODEFS.staticInit(); };
ERRNO_CODES = {
      'EPERM': 63,
      'ENOENT': 44,
      'ESRCH': 71,
      'EINTR': 27,
      'EIO': 29,
      'ENXIO': 60,
      'E2BIG': 1,
      'ENOEXEC': 45,
      'EBADF': 8,
      'ECHILD': 12,
      'EAGAIN': 6,
      'EWOULDBLOCK': 6,
      'ENOMEM': 48,
      'EACCES': 2,
      'EFAULT': 21,
      'ENOTBLK': 105,
      'EBUSY': 10,
      'EEXIST': 20,
      'EXDEV': 75,
      'ENODEV': 43,
      'ENOTDIR': 54,
      'EISDIR': 31,
      'EINVAL': 28,
      'ENFILE': 41,
      'EMFILE': 33,
      'ENOTTY': 59,
      'ETXTBSY': 74,
      'EFBIG': 22,
      'ENOSPC': 51,
      'ESPIPE': 70,
      'EROFS': 69,
      'EMLINK': 34,
      'EPIPE': 64,
      'EDOM': 18,
      'ERANGE': 68,
      'ENOMSG': 49,
      'EIDRM': 24,
      'ECHRNG': 106,
      'EL2NSYNC': 156,
      'EL3HLT': 107,
      'EL3RST': 108,
      'ELNRNG': 109,
      'EUNATCH': 110,
      'ENOCSI': 111,
      'EL2HLT': 112,
      'EDEADLK': 16,
      'ENOLCK': 46,
      'EBADE': 113,
      'EBADR': 114,
      'EXFULL': 115,
      'ENOANO': 104,
      'EBADRQC': 103,
      'EBADSLT': 102,
      'EDEADLOCK': 16,
      'EBFONT': 101,
      'ENOSTR': 100,
      'ENODATA': 116,
      'ETIME': 117,
      'ENOSR': 118,
      'ENONET': 119,
      'ENOPKG': 120,
      'EREMOTE': 121,
      'ENOLINK': 47,
      'EADV': 122,
      'ESRMNT': 123,
      'ECOMM': 124,
      'EPROTO': 65,
      'EMULTIHOP': 36,
      'EDOTDOT': 125,
      'EBADMSG': 9,
      'ENOTUNIQ': 126,
      'EBADFD': 127,
      'EREMCHG': 128,
      'ELIBACC': 129,
      'ELIBBAD': 130,
      'ELIBSCN': 131,
      'ELIBMAX': 132,
      'ELIBEXEC': 133,
      'ENOSYS': 52,
      'ENOTEMPTY': 55,
      'ENAMETOOLONG': 37,
      'ELOOP': 32,
      'EOPNOTSUPP': 138,
      'EPFNOSUPPORT': 139,
      'ECONNRESET': 15,
      'ENOBUFS': 42,
      'EAFNOSUPPORT': 5,
      'EPROTOTYPE': 67,
      'ENOTSOCK': 57,
      'ENOPROTOOPT': 50,
      'ESHUTDOWN': 140,
      'ECONNREFUSED': 14,
      'EADDRINUSE': 3,
      'ECONNABORTED': 13,
      'ENETUNREACH': 40,
      'ENETDOWN': 38,
      'ETIMEDOUT': 73,
      'EHOSTDOWN': 142,
      'EHOSTUNREACH': 23,
      'EINPROGRESS': 26,
      'EALREADY': 7,
      'EDESTADDRREQ': 17,
      'EMSGSIZE': 35,
      'EPROTONOSUPPORT': 66,
      'ESOCKTNOSUPPORT': 137,
      'EADDRNOTAVAIL': 4,
      'ENETRESET': 39,
      'EISCONN': 30,
      'ENOTCONN': 53,
      'ETOOMANYREFS': 141,
      'EUSERS': 136,
      'EDQUOT': 19,
      'ESTALE': 72,
      'ENOTSUP': 138,
      'ENOMEDIUM': 148,
      'EILSEQ': 25,
      'EOVERFLOW': 61,
      'ECANCELED': 11,
      'ENOTRECOVERABLE': 56,
      'EOWNERDEAD': 62,
      'ESTRPIPE': 135,
    };;
function checkIncomingModuleAPI() {
  ignoredModuleProp('fetchSettings');
}
var wasmImports = {
  JsArray_Check: JsArray_Check,
  JsArray_Delete: JsArray_Delete,
  JsArray_Extend: JsArray_Extend,
  JsArray_Get: JsArray_Get,
  JsArray_New: JsArray_New,
  JsArray_Push: JsArray_Push,
  JsArray_Push_unchecked: JsArray_Push_unchecked,
  JsArray_Set: JsArray_Set,
  JsArray_ShallowCopy: JsArray_ShallowCopy,
  JsArray_Splice: JsArray_Splice,
  JsArray_count_helper: JsArray_count_helper,
  JsArray_index_helper: JsArray_index_helper,
  JsArray_inplace_repeat_js: JsArray_inplace_repeat_js,
  JsArray_repeat_js: JsArray_repeat_js,
  JsArray_reverse_helper: JsArray_reverse_helper,
  JsArray_slice: JsArray_slice,
  JsArray_slice_assign: JsArray_slice_assign,
  JsBuffer_DecodeString_js: JsBuffer_DecodeString_js,
  JsDoubleProxy_unwrap_helper: JsDoubleProxy_unwrap_helper,
  JsException_new_helper: JsException_new_helper,
  JsMap_GetIter_js: JsMap_GetIter_js,
  JsMap_New: JsMap_New,
  JsMap_Set: JsMap_Set,
  JsMap_clear_js: JsMap_clear_js,
  JsObjMap_GetIter_js: JsObjMap_GetIter_js,
  JsObjMap_ass_subscript_js: JsObjMap_ass_subscript_js,
  JsObjMap_contains_js: JsObjMap_contains_js,
  JsObjMap_length_js: JsObjMap_length_js,
  JsObjMap_subscript_js: JsObjMap_subscript_js,
  JsObject_DeleteString: JsObject_DeleteString,
  JsObject_Dir: JsObject_Dir,
  JsObject_Entries: JsObject_Entries,
  JsObject_GetString: JsObject_GetString,
  JsObject_Keys: JsObject_Keys,
  JsObject_New: JsObject_New,
  JsObject_SetString: JsObject_SetString,
  JsObject_Values: JsObject_Values,
  JsProxy_GetAsyncIter_js: JsProxy_GetAsyncIter_js,
  JsProxy_GetIter_js: JsProxy_GetIter_js,
  JsProxy_compute_typeflags: JsProxy_compute_typeflags,
  JsProxy_subscript_js: JsProxy_subscript_js,
  JsSet_Add: JsSet_Add,
  JsSet_New: JsSet_New,
  JsString_InternFromCString: JsString_InternFromCString,
  _JsArray_PostProcess_helper: _JsArray_PostProcess_helper,
  _JsArray_PushEntry_helper: _JsArray_PushEntry_helper,
  _PyCFunctionWithKeywords_TrampolineCall: _PyCFunctionWithKeywords_TrampolineCall,
  _PyImport_InitFunc_TrampolineCall: _PyImport_InitFunc_TrampolineCall,
  _Py_CheckEmscriptenSignals_Helper: _Py_CheckEmscriptenSignals_Helper,
  _Py_emscripten_runtime: _Py_emscripten_runtime,
  __assert_fail: ___assert_fail,
  __call_sighandler: ___call_sighandler,
  __emscripten_atomics_sleep: ___emscripten_atomics_sleep,
  __hiwire_deduplicate_delete: __hiwire_deduplicate_delete,
  __hiwire_deduplicate_get: __hiwire_deduplicate_get,
  __hiwire_deduplicate_new: __hiwire_deduplicate_new,
  __hiwire_deduplicate_set: __hiwire_deduplicate_set,
  __syscall__newselect: ___syscall__newselect,
  __syscall_accept4: ___syscall_accept4,
  __syscall_bind: ___syscall_bind,
  __syscall_chdir: ___syscall_chdir,
  __syscall_chmod: ___syscall_chmod,
  __syscall_connect: ___syscall_connect,
  __syscall_dup: ___syscall_dup,
  __syscall_dup3: ___syscall_dup3,
  __syscall_faccessat: ___syscall_faccessat,
  __syscall_fadvise64: ___syscall_fadvise64,
  __syscall_fchdir: ___syscall_fchdir,
  __syscall_fchmod: ___syscall_fchmod,
  __syscall_fchmodat: ___syscall_fchmodat,
  __syscall_fchown32: ___syscall_fchown32,
  __syscall_fchownat: ___syscall_fchownat,
  __syscall_fcntl64: ___syscall_fcntl64,
  __syscall_fdatasync: ___syscall_fdatasync,
  __syscall_fstat64: ___syscall_fstat64,
  __syscall_fstatfs64: ___syscall_fstatfs64,
  __syscall_ftruncate64: ___syscall_ftruncate64,
  __syscall_getcwd: ___syscall_getcwd,
  __syscall_getdents64: ___syscall_getdents64,
  __syscall_getpeername: ___syscall_getpeername,
  __syscall_getsockname: ___syscall_getsockname,
  __syscall_getsockopt: ___syscall_getsockopt,
  __syscall_ioctl: ___syscall_ioctl,
  __syscall_listen: ___syscall_listen,
  __syscall_lstat64: ___syscall_lstat64,
  __syscall_mkdirat: ___syscall_mkdirat,
  __syscall_newfstatat: ___syscall_newfstatat,
  __syscall_openat: ___syscall_openat,
  __syscall_pipe: ___syscall_pipe,
  __syscall_poll: ___syscall_poll,
  __syscall_readlinkat: ___syscall_readlinkat,
  __syscall_recvfrom: ___syscall_recvfrom,
  __syscall_recvmsg: ___syscall_recvmsg,
  __syscall_renameat: ___syscall_renameat,
  __syscall_rmdir: ___syscall_rmdir,
  __syscall_sendmsg: ___syscall_sendmsg,
  __syscall_sendto: ___syscall_sendto,
  __syscall_socket: ___syscall_socket,
  __syscall_stat64: ___syscall_stat64,
  __syscall_statfs64: ___syscall_statfs64,
  __syscall_symlink: ___syscall_symlink,
  __syscall_truncate64: ___syscall_truncate64,
  __syscall_unlinkat: ___syscall_unlinkat,
  __syscall_utimensat: ___syscall_utimensat,
  _agen_handle_result_js: _agen_handle_result_js,
  _emscripten_get_now_is_monotonic: __emscripten_get_now_is_monotonic,
  _gmtime_js: __gmtime_js,
  _localtime_js: __localtime_js,
  _mktime_js: __mktime_js,
  _mmap_js: __mmap_js,
  _msync_js: __msync_js,
  _munmap_js: __munmap_js,
  _python2js_add_to_cache: _python2js_add_to_cache,
  _python2js_addto_postprocess_list: _python2js_addto_postprocess_list,
  _python2js_buffer_inner: _python2js_buffer_inner,
  _python2js_cache_lookup: _python2js_cache_lookup,
  _python2js_destroy_cache: _python2js_destroy_cache,
  _python2js_handle_postprocess_list: _python2js_handle_postprocess_list,
  _python2js_ucs1: _python2js_ucs1,
  _python2js_ucs2: _python2js_ucs2,
  _python2js_ucs4: _python2js_ucs4,
  _setitimer_js: __setitimer_js,
  _tzset_js: __tzset_js,
  abort: _abort,
  array_to_js: array_to_js,
  console_error_obj: console_error_obj,
  create_once_callable: create_once_callable,
  create_promise_handles: create_promise_handles,
  descr_get_trampoline_call: descr_get_trampoline_call,
  descr_set_trampoline_call: descr_set_trampoline_call,
  destroy_jsarray_entries: destroy_jsarray_entries,
  destroy_proxies: destroy_proxies,
  destroy_proxies_js: destroy_proxies_js,
  destroy_proxy: destroy_proxy,
  emscripten_asm_const_int: _emscripten_asm_const_int,
  emscripten_date_now: _emscripten_date_now,
  emscripten_err: _emscripten_err,
  emscripten_exit_with_live_runtime: _emscripten_exit_with_live_runtime,
  emscripten_get_heap_max: _emscripten_get_heap_max,
  emscripten_get_now: _emscripten_get_now,
  emscripten_get_now_res: _emscripten_get_now_res,
  emscripten_memcpy_big: _emscripten_memcpy_big,
  emscripten_resize_heap: _emscripten_resize_heap,
  environ_get: _environ_get,
  environ_sizes_get: _environ_sizes_get,
  exit: _exit,
  fail_test: fail_test,
  fd_close: _fd_close,
  fd_fdstat_get: _fd_fdstat_get,
  fd_pread: _fd_pread,
  fd_pwrite: _fd_pwrite,
  fd_read: _fd_read,
  fd_seek: _fd_seek,
  fd_sync: _fd_sync,
  fd_write: _fd_write,
  ffi_call_js: ffi_call_js,
  ffi_closure_alloc_js: ffi_closure_alloc_js,
  ffi_closure_free_js: ffi_closure_free_js,
  ffi_prep_closure_loc_js: ffi_prep_closure_loc_js,
  gc_register_proxies: gc_register_proxies,
  get_async_js_call_done_callback: get_async_js_call_done_callback,
  getaddrinfo: _getaddrinfo,
  getentropy: _getentropy,
  gethostbyaddr: _gethostbyaddr,
  gethostbyname: _gethostbyname,
  getnameinfo: _getnameinfo,
  getprotobyname: _getprotobyname,
  handle_next_result_js: handle_next_result_js,
  hiwire_CallMethod: hiwire_CallMethod,
  hiwire_CallMethod_NoArgs: hiwire_CallMethod_NoArgs,
  hiwire_CallMethod_OneArg: hiwire_CallMethod_OneArg,
  hiwire_assign_from_ptr: hiwire_assign_from_ptr,
  hiwire_assign_to_ptr: hiwire_assign_to_ptr,
  hiwire_call_OneArg: hiwire_call_OneArg,
  hiwire_call_bound: hiwire_call_bound,
  hiwire_construct: hiwire_construct,
  hiwire_constructor_name: hiwire_constructor_name,
  hiwire_double: hiwire_double,
  hiwire_equal: hiwire_equal,
  hiwire_get_bool: hiwire_get_bool,
  hiwire_get_buffer_info: hiwire_get_buffer_info,
  hiwire_get_length_helper: hiwire_get_length_helper,
  hiwire_get_length_string: hiwire_get_length_string,
  hiwire_greater_than: hiwire_greater_than,
  hiwire_greater_than_equal: hiwire_greater_than_equal,
  hiwire_init_js: hiwire_init_js,
  hiwire_int: hiwire_int,
  hiwire_int_from_digits: hiwire_int_from_digits,
  hiwire_into_file: hiwire_into_file,
  hiwire_invalid_ref: hiwire_invalid_ref,
  hiwire_is_async_generator: hiwire_is_async_generator,
  hiwire_is_comlink_proxy: hiwire_is_comlink_proxy,
  hiwire_is_function: hiwire_is_function,
  hiwire_is_generator: hiwire_is_generator,
  hiwire_is_promise: hiwire_is_promise,
  hiwire_less_than: hiwire_less_than,
  hiwire_less_than_equal: hiwire_less_than_equal,
  hiwire_not_equal: hiwire_not_equal,
  hiwire_read_from_file: hiwire_read_from_file,
  hiwire_resolve_promise: hiwire_resolve_promise,
  hiwire_reversed_iterator: hiwire_reversed_iterator,
  hiwire_string_utf8: hiwire_string_utf8,
  hiwire_throw_error: hiwire_throw_error,
  hiwire_to_bool: hiwire_to_bool,
  hiwire_to_string: hiwire_to_string,
  hiwire_typeof: hiwire_typeof,
  hiwire_write_to_file: hiwire_write_to_file,
  js2python_convert: js2python_convert,
  js2python_immutable_js: js2python_immutable_js,
  js2python_init: js2python_init,
  js2python_js: js2python_js,
  new_error: new_error,
  proc_exit: _proc_exit,
  proxy_cache_get: proxy_cache_get,
  proxy_cache_set: proxy_cache_set,
  pyodide_js_init: pyodide_js_init,
  pyproxy_AsPyObject: pyproxy_AsPyObject,
  pyproxy_Check: pyproxy_Check,
  pyproxy_new: pyproxy_new,
  pyproxy_new_ex: pyproxy_new_ex,
  python2js__default_converter_js: python2js__default_converter_js,
  python2js_buffer_init: python2js_buffer_init,
  python2js_custom__create_jscontext: python2js_custom__create_jscontext,
  strftime: _strftime,
  system: _system,
  throw_no_gil: throw_no_gil,
  wrap_async_generator: wrap_async_generator,
  wrap_generator: wrap_generator
};
var wasmExports = createWasm();
var ___wasm_call_ctors = createExportWrapper('__wasm_call_ctors');
var _PyObject_Size = Module['_PyObject_Size'] = createExportWrapper('PyObject_Size');
var _PyBuffer_Release = Module['_PyBuffer_Release'] = createExportWrapper('PyBuffer_Release');
var _PyObject_GetIter = Module['_PyObject_GetIter'] = createExportWrapper('PyObject_GetIter');
var _PyObject_GetAIter = Module['_PyObject_GetAIter'] = createExportWrapper('PyObject_GetAIter');
var _PyFloat_FromDouble = Module['_PyFloat_FromDouble'] = createExportWrapper('PyFloat_FromDouble');
var _PyList_New = Module['_PyList_New'] = createExportWrapper('PyList_New');
var _PyList_SetItem = Module['_PyList_SetItem'] = createExportWrapper('PyList_SetItem');
var _PyLong_FromDouble = Module['_PyLong_FromDouble'] = createExportWrapper('PyLong_FromDouble');
var __PyLong_FromByteArray = Module['__PyLong_FromByteArray'] = createExportWrapper('_PyLong_FromByteArray');
var _PyDict_New = Module['_PyDict_New'] = createExportWrapper('PyDict_New');
var _PyDict_SetItem = Module['_PyDict_SetItem'] = createExportWrapper('PyDict_SetItem');
var _Py_IncRef = Module['_Py_IncRef'] = createExportWrapper('Py_IncRef');
var _Py_DecRef = Module['_Py_DecRef'] = createExportWrapper('Py_DecRef');
var _PyMem_Free = Module['_PyMem_Free'] = createExportWrapper('PyMem_Free');
var _PySet_New = Module['_PySet_New'] = createExportWrapper('PySet_New');
var _PySet_Add = Module['_PySet_Add'] = createExportWrapper('PySet_Add');
var _PyUnicode_New = Module['_PyUnicode_New'] = createExportWrapper('PyUnicode_New');
var _PyEval_SaveThread = Module['_PyEval_SaveThread'] = createExportWrapper('PyEval_SaveThread');
var _PyEval_RestoreThread = Module['_PyEval_RestoreThread'] = createExportWrapper('PyEval_RestoreThread');
var _PyErr_SetString = Module['_PyErr_SetString'] = createExportWrapper('PyErr_SetString');
var _PyErr_Occurred = Module['_PyErr_Occurred'] = createExportWrapper('PyErr_Occurred');
var _PyErr_Clear = Module['_PyErr_Clear'] = createExportWrapper('PyErr_Clear');
var _PyGILState_Check = Module['_PyGILState_Check'] = createExportWrapper('PyGILState_Check');
var _PyErr_Print = Module['_PyErr_Print'] = createExportWrapper('PyErr_Print');
var _PyRun_SimpleString = Module['_PyRun_SimpleString'] = createExportWrapper('PyRun_SimpleString');
var __PyTraceback_Add = Module['__PyTraceback_Add'] = createExportWrapper('_PyTraceback_Add');
var _PyErr_CheckSignals = Module['_PyErr_CheckSignals'] = createExportWrapper('PyErr_CheckSignals');
var __PyErr_CheckSignals = Module['__PyErr_CheckSignals'] = createExportWrapper('_PyErr_CheckSignals');
var _dump_traceback = Module['_dump_traceback'] = createExportWrapper('dump_traceback');
var _set_error = Module['_set_error'] = createExportWrapper('set_error');
var _restore_sys_last_exception = Module['_restore_sys_last_exception'] = createExportWrapper('restore_sys_last_exception');
var _wrap_exception = Module['_wrap_exception'] = createExportWrapper('wrap_exception');
var _pythonexc2js = Module['_pythonexc2js'] = createExportWrapper('pythonexc2js');
var _JsString_FromId = Module['_JsString_FromId'] = createExportWrapper('JsString_FromId');
var _PyUnicode_Data = Module['_PyUnicode_Data'] = createExportWrapper('PyUnicode_Data');
var __js2python_none = Module['__js2python_none'] = createExportWrapper('_js2python_none');
var __js2python_true = Module['__js2python_true'] = createExportWrapper('_js2python_true');
var __js2python_false = Module['__js2python_false'] = createExportWrapper('_js2python_false');
var __js2python_pyproxy = Module['__js2python_pyproxy'] = createExportWrapper('_js2python_pyproxy');
var _js2python_immutable = Module['_js2python_immutable'] = createExportWrapper('js2python_immutable');
var _js2python = Module['_js2python'] = createExportWrapper('js2python');
var __agen_handle_result_js_c = Module['__agen_handle_result_js_c'] = createExportWrapper('_agen_handle_result_js_c');
var _JsBuffer_CopyIntoMemoryView = Module['_JsBuffer_CopyIntoMemoryView'] = createExportWrapper('JsBuffer_CopyIntoMemoryView');
var _JsProxy_create = Module['_JsProxy_create'] = createExportWrapper('JsProxy_create');
var _JsProxy_Check = Module['_JsProxy_Check'] = createExportWrapper('JsProxy_Check');
var _check_gil = Module['_check_gil'] = createExportWrapper('check_gil');
var _pyproxy_getflags = Module['_pyproxy_getflags'] = createExportWrapper('pyproxy_getflags');
var __pyproxy_repr = Module['__pyproxy_repr'] = createExportWrapper('_pyproxy_repr');
var __pyproxy_type = Module['__pyproxy_type'] = createExportWrapper('_pyproxy_type');
var __pyproxy_hasattr = Module['__pyproxy_hasattr'] = createExportWrapper('_pyproxy_hasattr');
var __pyproxy_getattr = Module['__pyproxy_getattr'] = createExportWrapper('_pyproxy_getattr');
var __pyproxy_setattr = Module['__pyproxy_setattr'] = createExportWrapper('_pyproxy_setattr');
var __pyproxy_delattr = Module['__pyproxy_delattr'] = createExportWrapper('_pyproxy_delattr');
var __pyproxy_getitem = Module['__pyproxy_getitem'] = createExportWrapper('_pyproxy_getitem');
var __pyproxy_setitem = Module['__pyproxy_setitem'] = createExportWrapper('_pyproxy_setitem');
var __pyproxy_delitem = Module['__pyproxy_delitem'] = createExportWrapper('_pyproxy_delitem');
var __pyproxy_slice_assign = Module['__pyproxy_slice_assign'] = createExportWrapper('_pyproxy_slice_assign');
var __pyproxy_pop = Module['__pyproxy_pop'] = createExportWrapper('_pyproxy_pop');
var __pyproxy_contains = Module['__pyproxy_contains'] = createExportWrapper('_pyproxy_contains');
var __pyproxy_ownKeys = Module['__pyproxy_ownKeys'] = createExportWrapper('_pyproxy_ownKeys');
var __pyproxy_apply = Module['__pyproxy_apply'] = createExportWrapper('_pyproxy_apply');
var __iscoroutinefunction = Module['__iscoroutinefunction'] = createExportWrapper('_iscoroutinefunction');
var __pyproxy_iter_next = Module['__pyproxy_iter_next'] = createExportWrapper('_pyproxy_iter_next');
var __pyproxyGen_Send = Module['__pyproxyGen_Send'] = createExportWrapper('_pyproxyGen_Send');
var __pyproxyGen_return = Module['__pyproxyGen_return'] = createExportWrapper('_pyproxyGen_return');
var __pyproxyGen_throw = Module['__pyproxyGen_throw'] = createExportWrapper('_pyproxyGen_throw');
var __pyproxyGen_asend = Module['__pyproxyGen_asend'] = createExportWrapper('_pyproxyGen_asend');
var __pyproxyGen_areturn = Module['__pyproxyGen_areturn'] = createExportWrapper('_pyproxyGen_areturn');
var __pyproxyGen_athrow = Module['__pyproxyGen_athrow'] = createExportWrapper('_pyproxyGen_athrow');
var __pyproxy_aiter_next = Module['__pyproxy_aiter_next'] = createExportWrapper('_pyproxy_aiter_next');
var __pyproxy_ensure_future = Module['__pyproxy_ensure_future'] = createExportWrapper('_pyproxy_ensure_future');
var __pyproxy_get_buffer = Module['__pyproxy_get_buffer'] = createExportWrapper('_pyproxy_get_buffer');
var __python2js = Module['__python2js'] = createExportWrapper('_python2js');
var _python2js = Module['_python2js'] = createExportWrapper('python2js');
var _python2js_with_depth = Module['_python2js_with_depth'] = createExportWrapper('python2js_with_depth');
var _python2js_custom = Module['_python2js_custom'] = createExportWrapper('python2js_custom');
var _pyodide_export = Module['_pyodide_export'] = createExportWrapper('pyodide_export');
var _py_version_major = Module['_py_version_major'] = createExportWrapper('py_version_major');
var _py_version_minor = Module['_py_version_minor'] = createExportWrapper('py_version_minor');
var _py_version_micro = Module['_py_version_micro'] = createExportWrapper('py_version_micro');
var _hiwire_get = Module['_hiwire_get'] = createExportWrapper('hiwire_get');
var _hiwire_incref = Module['_hiwire_incref'] = createExportWrapper('hiwire_incref');
var _hiwire_intern = Module['_hiwire_intern'] = createExportWrapper('hiwire_intern');
var _hiwire_new = Module['_hiwire_new'] = createExportWrapper('hiwire_new');
var _hiwire_num_refs = Module['_hiwire_num_refs'] = createExportWrapper('hiwire_num_refs');
var _hiwire_decref = Module['_hiwire_decref'] = createExportWrapper('hiwire_decref');
var _hiwire_pop = Module['_hiwire_pop'] = createExportWrapper('hiwire_pop');
var _main = Module['_main'] = createExportWrapper('__main_argc_argv');
var _malloc = createExportWrapper('malloc');
var _free = Module['_free'] = createExportWrapper('free');
var _fflush = Module['_fflush'] = createExportWrapper('fflush');
var ___errno_location = createExportWrapper('__errno_location');
var _ntohs = createExportWrapper('ntohs');
var _htons = createExportWrapper('htons');
var _htonl = createExportWrapper('htonl');
var _emscripten_builtin_memalign = createExportWrapper('emscripten_builtin_memalign');
var __emscripten_timeout = createExportWrapper('_emscripten_timeout');
var _emscripten_stack_init = () => (_emscripten_stack_init = wasmExports['emscripten_stack_init'])();
var _emscripten_stack_get_free = () => (_emscripten_stack_get_free = wasmExports['emscripten_stack_get_free'])();
var _emscripten_stack_get_base = () => (_emscripten_stack_get_base = wasmExports['emscripten_stack_get_base'])();
var _emscripten_stack_get_end = () => (_emscripten_stack_get_end = wasmExports['emscripten_stack_get_end'])();
var stackSave = createExportWrapper('stackSave');
var stackRestore = createExportWrapper('stackRestore');
var stackAlloc = createExportWrapper('stackAlloc');
var _emscripten_stack_get_current = () => (_emscripten_stack_get_current = wasmExports['emscripten_stack_get_current'])();
var _Py_EMSCRIPTEN_SIGNAL_HANDLING = Module['_Py_EMSCRIPTEN_SIGNAL_HANDLING'] = 8771088;
var _Js_true = Module['_Js_true'] = 7613168;
var _Js_false = Module['_Js_false'] = 7613172;
var _Js_undefined = Module['_Js_undefined'] = 7613176;
var _Js_null = Module['_Js_null'] = 7613180;
var _buffer_struct_size = Module['_buffer_struct_size'] = 8651520;
var ___start_em_js = Module['___start_em_js'] = 8655440;
var ___stop_em_js = Module['___stop_em_js'] = 8770797;

// include: postamble.js
// === Auto-generated postamble setup entry stuff ===

Module['wasmMemory'] = wasmMemory;
Module['growMemory'] = growMemory;
Module['ERRNO_CODES'] = ERRNO_CODES;
Module['STACK_SIZE'] = STACK_SIZE;
Module['PATH'] = PATH;
Module['stringToNewUTF8'] = stringToNewUTF8;
Module['preloadPlugins'] = preloadPlugins;
Module['FS'] = FS;
Module['LZ4'] = LZ4;
var missingLibrarySymbols = [
  'writeI53ToI64',
  'writeI53ToI64Clamped',
  'writeI53ToI64Signaling',
  'writeI53ToU64Clamped',
  'writeI53ToU64Signaling',
  'readI53FromU64',
  'convertI32PairToI53',
  'convertI32PairToI53Checked',
  'convertU32PairToI53',
  'getCallstack',
  'emscriptenLog',
  'convertPCtoSourceLocation',
  'runMainThreadEmAsm',
  'jstoi_s',
  'listenOnce',
  'autoResumeAudioContext',
  'getDynCaller',
  'dynCall',
  'runtimeKeepalivePush',
  'runtimeKeepalivePop',
  'safeSetTimeout',
  'asmjsMangle',
  'handleAllocatorInit',
  'HandleAllocator',
  'getNativeTypeSize',
  'STACK_ALIGN',
  'POINTER_SIZE',
  'ASSERTIONS',
  'getCFunc',
  'ccall',
  'cwrap',
  'updateTableMap',
  'getFunctionAddress',
  'addFunction',
  'removeFunction',
  'reallyNegative',
  'unSign',
  'strLen',
  'reSign',
  'formatString',
  'intArrayToString',
  'AsciiToString',
  'UTF16ToString',
  'stringToUTF16',
  'lengthBytesUTF16',
  'UTF32ToString',
  'stringToUTF32',
  'lengthBytesUTF32',
  'registerKeyEventCallback',
  'maybeCStringToJsString',
  'findEventTarget',
  'findCanvasEventTarget',
  'getBoundingClientRect',
  'fillMouseEventData',
  'registerMouseEventCallback',
  'registerWheelEventCallback',
  'registerUiEventCallback',
  'registerFocusEventCallback',
  'fillDeviceOrientationEventData',
  'registerDeviceOrientationEventCallback',
  'fillDeviceMotionEventData',
  'registerDeviceMotionEventCallback',
  'screenOrientation',
  'fillOrientationChangeEventData',
  'registerOrientationChangeEventCallback',
  'fillFullscreenChangeEventData',
  'registerFullscreenChangeEventCallback',
  'JSEvents_requestFullscreen',
  'JSEvents_resizeCanvasForFullscreen',
  'registerRestoreOldStyle',
  'hideEverythingExceptGivenElement',
  'restoreHiddenElements',
  'setLetterbox',
  'softFullscreenResizeWebGLRenderTarget',
  'doRequestFullscreen',
  'fillPointerlockChangeEventData',
  'registerPointerlockChangeEventCallback',
  'registerPointerlockErrorEventCallback',
  'requestPointerLock',
  'fillVisibilityChangeEventData',
  'registerVisibilityChangeEventCallback',
  'registerTouchEventCallback',
  'fillGamepadEventData',
  'registerGamepadEventCallback',
  'registerBeforeUnloadEventCallback',
  'fillBatteryEventData',
  'battery',
  'registerBatteryEventCallback',
  'setCanvasElementSize',
  'getCanvasElementSize',
  'jsStackTrace',
  'stackTrace',
  'checkWasiClock',
  'wasiRightsToMuslOFlags',
  'wasiOFlagsToMuslOFlags',
  'createDyncallWrapper',
  'setImmediateWrapped',
  'clearImmediateWrapped',
  'polyfillSetImmediate',
  'getPromise',
  'makePromise',
  'idsToPromises',
  'makePromiseCallback',
  'ExceptionInfo',
  'findMatchingCatch',
  'setMainLoop',
  '_setNetworkCallback',
  'heapObjectForWebGLType',
  'heapAccessShiftForWebGLHeap',
  'webgl_enable_ANGLE_instanced_arrays',
  'webgl_enable_OES_vertex_array_object',
  'webgl_enable_WEBGL_draw_buffers',
  'webgl_enable_WEBGL_multi_draw',
  'emscriptenWebGLGet',
  'computeUnpackAlignedImageSize',
  'colorChannelsInGlTextureFormat',
  'emscriptenWebGLGetTexPixelData',
  '__glGenObject',
  'emscriptenWebGLGetUniform',
  'webglGetUniformLocation',
  'webglPrepareUniformLocationsBeforeFirstUse',
  'webglGetLeftBracePos',
  'emscriptenWebGLGetVertexAttrib',
  '__glGetActiveAttribOrUniform',
  'writeGLArray',
  'registerWebGlEventCallback',
  'runAndAbortIfError',
  'SDL_unicode',
  'SDL_ttfContext',
  'SDL_audio',
  'GLFW_Window',
  'ALLOC_NORMAL',
  'ALLOC_STACK',
  'allocate',
  'writeStringToMemory',
  'writeAsciiToMemory',
];
missingLibrarySymbols.forEach(missingLibrarySymbol)

var unexportedSymbols = [
  'run',
  'addOnPreRun',
  'addOnInit',
  'addOnPreMain',
  'addOnExit',
  'addOnPostRun',
  'addRunDependency',
  'removeRunDependency',
  'FS_createFolder',
  'FS_createPath',
  'FS_createDataFile',
  'FS_createLazyFile',
  'FS_createLink',
  'FS_createDevice',
  'FS_readFile',
  'FS_unlink',
  'out',
  'err',
  'callMain',
  'abort',
  'keepRuntimeAlive',
  'wasmTable',
  'wasmExports',
  'stackAlloc',
  'stackSave',
  'stackRestore',
  'getTempRet0',
  'setTempRet0',
  'writeStackCookie',
  'checkStackCookie',
  'readI53FromI64',
  'MAX_INT53',
  'MIN_INT53',
  'bigintToI53Checked',
  'ptrToString',
  'zeroMemory',
  'exitJS',
  'getHeapMax',
  'abortOnCannotGrowMemory',
  'ENV',
  'MONTH_DAYS_REGULAR',
  'MONTH_DAYS_LEAP',
  'MONTH_DAYS_REGULAR_CUMULATIVE',
  'MONTH_DAYS_LEAP_CUMULATIVE',
  'isLeapYear',
  'ydayFromDate',
  'arraySum',
  'addDays',
  'ERRNO_MESSAGES',
  'setErrNo',
  'inetPton4',
  'inetNtop4',
  'inetPton6',
  'inetNtop6',
  'readSockaddr',
  'writeSockaddr',
  'DNS',
  'getHostByName',
  'Protocols',
  'Sockets',
  'initRandomFill',
  'randomFill',
  'timers',
  'warnOnce',
  'UNWIND_CACHE',
  'readEmAsmArgsArray',
  'readEmAsmArgs',
  'runEmAsmFunction',
  'jstoi_q',
  'getExecutableName',
  'setWasmTableEntry',
  'handleException',
  'callUserCallback',
  'maybeExit',
  'asyncLoad',
  'alignMemory',
  'mmapAlloc',
  'uleb128Encode',
  'sigToWasmTypes',
  'generateFuncType',
  'convertJsFunctionToWasm',
  'freeTableIndexes',
  'functionsInTableMap',
  'getEmptyTableSlot',
  'setValue',
  'getValue',
  'PATH_FS',
  'UTF8Decoder',
  'UTF8ArrayToString',
  'UTF8ToString',
  'stringToUTF8Array',
  'stringToUTF8',
  'lengthBytesUTF8',
  'intArrayFromString',
  'stringToAscii',
  'UTF16Decoder',
  'stringToUTF8OnStack',
  'writeArrayToMemory',
  'JSEvents',
  'specialHTMLTargets',
  'currentFullscreenStrategy',
  'restoreOldWindowedStyle',
  'demangle',
  'demangleAll',
  'ExitStatus',
  'getEnvStrings',
  'doReadv',
  'doWritev',
  'promiseMap',
  'uncaughtExceptionCount',
  'exceptionLast',
  'exceptionCaught',
  'Browser',
  'wget',
  'SYSCALLS',
  'getSocketFromFD',
  'getSocketAddress',
  'FS_createPreloadedFile',
  'FS_modeStringToFlags',
  'FS_getMode',
  'FS_stdin_getChar_buffer',
  'FS_stdin_getChar',
  'MEMFS',
  'TTY',
  'PIPEFS',
  'SOCKFS',
  'tempFixedLengthArray',
  'miniTempWebGLFloatBuffers',
  'miniTempWebGLIntBuffers',
  'GL',
  'emscripten_webgl_power_preferences',
  'AL',
  'GLUT',
  'EGL',
  'GLEW',
  'IDBStore',
  'SDL',
  'SDL_gfx',
  'GLFW',
  'allocateUTF8',
  'allocateUTF8OnStack',
  'NODEFS',
];
unexportedSymbols.forEach(unexportedRuntimeSymbol);



var calledRun;

dependenciesFulfilled = function runCaller() {
  // If run has never been called, and we should call run (INVOKE_RUN is true, and Module.noInitialRun is not false)
  if (!calledRun) run();
  if (!calledRun) dependenciesFulfilled = runCaller; // try this again later, after new deps are fulfilled
};

function callMain(args = []) {
  assert(runDependencies == 0, 'cannot call main when async dependencies remain! (listen on Module["onRuntimeInitialized"])');
  assert(__ATPRERUN__.length == 0, 'cannot call main when preRun functions remain to be called');

  var entryFunction = _main;

  args.unshift(thisProgram);

  var argc = args.length;
  var argv = stackAlloc((argc + 1) * 4);
  var argv_ptr = argv;
  args.forEach((arg) => {
    HEAPU32[((argv_ptr)>>2)] = stringToUTF8OnStack(arg);
    argv_ptr += 4;
  });
  HEAPU32[((argv_ptr)>>2)] = 0;

  try {

    var ret = entryFunction(argc, argv);

    // if we're not running an evented main loop, it's time to exit
    exitJS(ret, /* implicit = */ true);
    return ret;
  }
  catch (e) {
    return handleException(e);
  }
}

function stackCheckInit() {
  // This is normally called automatically during __wasm_call_ctors but need to
  // get these values before even running any of the ctors so we call it redundantly
  // here.
  _emscripten_stack_init();
  // TODO(sbc): Move writeStackCookie to native to to avoid this.
  writeStackCookie();
}

function run(args = arguments_) {

  if (runDependencies > 0) {
    return;
  }

    stackCheckInit();

  preRun();

  // a preRun added a dependency, run will be called later
  if (runDependencies > 0) {
    return;
  }

  function doRun() {
    // run may have just been called through dependencies being fulfilled just in this very frame,
    // or while the async setStatus time below was happening
    if (calledRun) return;
    calledRun = true;
    Module['calledRun'] = true;

    if (ABORT) return;

    initRuntime();

    preMain();

    readyPromiseResolve(Module);
    if (Module['onRuntimeInitialized']) Module['onRuntimeInitialized']();

    if (shouldRunNow) callMain(args);

    postRun();
  }

  if (Module['setStatus']) {
    Module['setStatus']('Running...');
    setTimeout(function() {
      setTimeout(function() {
        Module['setStatus']('');
      }, 1);
      doRun();
    }, 1);
  } else
  {
    doRun();
  }
  checkStackCookie();
}

function checkUnflushedContent() {
  // Compiler settings do not allow exiting the runtime, so flushing
  // the streams is not possible. but in ASSERTIONS mode we check
  // if there was something to flush, and if so tell the user they
  // should request that the runtime be exitable.
  // Normally we would not even include flush() at all, but in ASSERTIONS
  // builds we do so just for this check, and here we see if there is any
  // content to flush, that is, we check if there would have been
  // something a non-ASSERTIONS build would have not seen.
  // How we flush the streams depends on whether we are in SYSCALLS_REQUIRE_FILESYSTEM=0
  // mode (which has its own special function for this; otherwise, all
  // the code is inside libc)
  var oldOut = out;
  var oldErr = err;
  var has = false;
  out = err = (x) => {
    has = true;
  }
  try { // it doesn't matter if it fails
    _fflush(0);
    // also flush in the JS FS layer
    ['stdout', 'stderr'].forEach(function(name) {
      var info = FS.analyzePath('/dev/' + name);
      if (!info) return;
      var stream = info.object;
      var rdev = stream.rdev;
      var tty = TTY.ttys[rdev];
      if (tty && tty.output && tty.output.length) {
        has = true;
      }
    });
  } catch(e) {}
  out = oldOut;
  err = oldErr;
  if (has) {
    warnOnce('stdio streams had content in them that was not flushed. you should set EXIT_RUNTIME to 1 (see the Emscripten FAQ), or make sure to emit a newline when you printf etc.');
  }
}

if (Module['preInit']) {
  if (typeof Module['preInit'] == 'function') Module['preInit'] = [Module['preInit']];
  while (Module['preInit'].length > 0) {
    Module['preInit'].pop()();
  }
}

// shouldRunNow refers to calling main(), not run().
var shouldRunNow = true;

if (Module['noInitialRun']) shouldRunNow = false;

run();


// end include: postamble.js


  return moduleArg.ready
}

);
})();
export default Module;