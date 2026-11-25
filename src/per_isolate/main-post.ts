'use strict';

// Secondary bootstrap. To be run after restoring the snapshot.
// This section must install the stuff that is compat-flag dependent.

const { ObjectDefineProperties, ObjectGetPrototypeOf } = primordials;

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

// EventSource is always a global. Its module captures built-ins off
// globalThis, the streams reader among them, so it is loaded here, before any
// user code can patch them, and after the streams install above. It reads the
// flags as it loads, so it cannot load in main.ts. Installed as
// JSG_NESTED_TYPE installs a global class: on the global scope's prototype,
// with all three attributes true, and under spec_compliant_property_attributes
// also as an own property of the global, non-enumerable per Web IDL.
{
  const { EventSource } = require('eventsource/eventsource');
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
