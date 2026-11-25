'use strict';

// Secondary bootstrap. To be run after restoring the snapshot.
// This section must install the stuff that is compat-flag dependent.
//
// There are several pseudo-globals available in this script:
// - `globalThis`: the global object of the context being initialized.
// - `compatFlags`: an object containing compatibility flags enabled for the context.
// - `autogates`: an object containing autogates enabled for the context.
// - `require`: a function to load other per-isolate bootstrap scripts.
// - `module`: the commonjs-style module object for this script, used for exporting values
// - `exports`: the commonjs-style exports object for this script, used for exporting values
// - `primordials`: an object containing references to built-in methods and constructors,
//   used to prevent prototype pollution

// Everything installed here is intentionally not included in the zygote
// snapshot.

const { ObjectDefineProperties, ObjectGetPrototypeOf } = primordials;

const { defineLazyDataProperty } = utils;

// The bootstrap classes carry Symbol.toStringTag whatever set_tostring_tag
// says (see markAsRuntimeClass()). This takes it off those loaded here.
const { removeToStringTagUnlessFlagged } = require('runtime-class');

const globalPrototype = ObjectGetPrototypeOf(globalThis) as object;

if (
  autogates['ts-web-streams'] ||
  compatFlags['typescript_implemented_streams']
) {
  const {
    installWebStreams,
    maybeInstallWebFileSystemStreams,
    maybeInstallDrainingReader,
  } = require('webstreams/installer');
  // Under the ts-web-streams autogate, main.ts has already installed the
  // streams; installWebStreams() does nothing the second time.
  installWebStreams();
  maybeInstallWebFileSystemStreams();
  maybeInstallDrainingReader();
}

// MessageChannel and MessagePort are loaded on first use, since few Workers
// use them: through the globals defined here, or through
// cloudflare-internal:messagechannel (MessageChannelModule in
// api/messagechannel.h), which node:worker_threads uses. Whichever comes first
// loads the module; the other finds it in the require() cache.
if (compatFlags['expose_global_message_channel']) {
  const specifier = 'messagechannel/messagechannel';
  // Defined as JSG_NESTED_TYPE defines a global class: always on the global
  // scope's prototype, with all three attributes true, and under
  // spec_compliant_property_attributes also as own properties of the global,
  // non-enumerable per Web IDL. Each is a lazy data property, an ordinary data
  // property once read.
  defineLazyDataProperty(globalPrototype, 'MessageChannel', specifier, true);
  defineLazyDataProperty(globalPrototype, 'MessagePort', specifier, true);
  if (compatFlags['spec_compliant_property_attributes']) {
    defineLazyDataProperty(globalThis, 'MessageChannel', specifier, false);
    defineLazyDataProperty(globalThis, 'MessagePort', specifier, false);
  }
}

// EventSource is always a global. Its module captures built-ins off
// globalThis, the streams reader among them, so it is loaded here, before any
// user code can patch them, and after the streams install above. It reads the
// flags as it loads, so it cannot load in main.ts. Installed as
// JSG_NESTED_TYPE installs a global class: on the global scope's prototype,
// with all three attributes true, and under spec_compliant_property_attributes
// also as an own property of the global, non-enumerable per Web IDL.
{
  const { EventSource } = require('eventsource/eventsource');
  removeToStringTagUnlessFlagged(EventSource.prototype);
  ObjectDefineProperties(globalPrototype, {
    __proto__: null,
    EventSource: {
      __proto__: null,
      configurable: true,
      enumerable: true,
      writable: true,
      value: EventSource,
    },
  });
  if (compatFlags['spec_compliant_property_attributes']) {
    ObjectDefineProperties(globalThis, {
      __proto__: null,
      EventSource: {
        __proto__: null,
        configurable: true,
        enumerable: false,
        writable: true,
        value: EventSource,
      },
    });
  }
}

// The TypeScript URL and URLSearchParams replace the C++ ones, which
// global-scope.h then leaves out. Installed as WebIDL installs an interface:
// own, non-enumerable properties of the global, and not on its prototype. The
// module reads the flags as it loads, the url_standard backend among them.
if (autogates['ts-url']) {
  const {
    URL,
    URLSearchParams,
    URLSearchParamsIteratorPrototype,
  } = require('url/url');
  // The C++ bridge's functions, which it finds already loaded.
  require('url/cpp_exports');
  removeToStringTagUnlessFlagged(URL.prototype);
  removeToStringTagUnlessFlagged(URLSearchParams.prototype);
  removeToStringTagUnlessFlagged(URLSearchParamsIteratorPrototype);
  ObjectDefineProperties(globalThis, {
    __proto__: null,
    URL: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: URL,
    },
    URLSearchParams: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: URLSearchParams,
    },
  });
}

// The TypeScript URLPattern replaces the C++ ones, which global-scope.h then
// leaves out, installed as URL is. The module reads the flags as it loads,
// the urlpattern_standard parser among them.
if (autogates['ts-url-pattern']) {
  const { URLPattern } = require('urlpattern/urlpattern');
  removeToStringTagUnlessFlagged(URLPattern.prototype);
  ObjectDefineProperties(globalThis, {
    __proto__: null,
    URLPattern: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: URLPattern,
    },
  });
}

// Internal-only: a cloneable class for testing the serialization protocol
// (serialization.ts), such as over RPC. Gated by an experimental flag that
// may never lose its experimental annotation.
if (compatFlags['expose_serialization_test_class']) {
  const { CloneableTestBox } = require('testing/cloneable-test-box');
  ObjectDefineProperties(globalThis, {
    __proto__: null,
    CloneableTestBox: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: CloneableTestBox,
    },
  });
}
