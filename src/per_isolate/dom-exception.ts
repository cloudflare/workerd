'use strict';

// The DOMException the bootstrap's modules create and recognize: the C++
// class (jsg/dom-exception.h) that the global scope installs, captured as
// this module loads, before user code can replace the global. main.ts loads
// it before any other module that may use it.

const { ObjectFreeze } = primordials;

const DOMException = globalThis.DOMException;

export type DomExceptionModule = {
  DOMException: typeof DOMException;
};

module.exports = ObjectFreeze({ DOMException });
