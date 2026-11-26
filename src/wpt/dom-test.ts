// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'common.js': {
    comment: 'Helper library for the document tests, not a test',
    omittedTests: true,
  },
  'constants.js': {
    comment: 'Helper library for the document tests, not a test',
    omittedTests: true,
  },
  'idlharness.any.js': {
    comment:
      'Event and CustomEvent lack initEvent(), initCustomEvent() and a null target and srcElement, isTrusted is a data property, and the CustomEvent and AbortSignal interface objects do not inherit from Event and EventTarget. The other interfaces in the DOM IDL need a document.',
    expectedFailures: [
      'Event interface: operation initEvent(DOMString, optional boolean, optional boolean)',
      'Event interface: new Event("foo") must inherit property "target" with the proper type',
      'Event interface: new Event("foo") must inherit property "srcElement" with the proper type',
      'Event interface: new Event("foo") must have own property "isTrusted"',
      'Event interface: new Event("foo") must inherit property "initEvent(DOMString, optional boolean, optional boolean)" with the proper type',
      'Event interface: calling initEvent(DOMString, optional boolean, optional boolean) on new Event("foo") with too few arguments must throw TypeError',
      'CustomEvent interface: existence and properties of interface object',
      'CustomEvent interface: operation initCustomEvent(DOMString, optional boolean, optional boolean, optional any)',
      'CustomEvent interface: new CustomEvent("foo") must inherit property "initCustomEvent(DOMString, optional boolean, optional boolean, optional any)" with the proper type',
      'CustomEvent interface: calling initCustomEvent(DOMString, optional boolean, optional boolean, optional any) on new CustomEvent("foo") with too few arguments must throw TypeError',
      'Event interface: new CustomEvent("foo") must inherit property "target" with the proper type',
      'Event interface: new CustomEvent("foo") must inherit property "srcElement" with the proper type',
      'Event interface: new CustomEvent("foo") must have own property "isTrusted"',
      'Event interface: new CustomEvent("foo") must inherit property "initEvent(DOMString, optional boolean, optional boolean)" with the proper type',
      'Event interface: calling initEvent(DOMString, optional boolean, optional boolean) on new CustomEvent("foo") with too few arguments must throw TypeError',
      'AbortSignal interface: existence and properties of interface object',
      'Window interface: attribute event',
    ],
    omittedTests: [
      /^(AbstractRange|Attr|CDATASection|CharacterData|Comment|DOMImplementation|DOMTokenList|Document|DocumentFragment|DocumentType|Element|HTMLCollection|MutationObserver|MutationRecord|NamedNodeMap|Node|NodeFilter|NodeIterator|NodeList|NodeRange|ProcessingInstruction|Range|ShadowRoot|StaticRange|Text|TreeWalker|XMLDocument|XPathEvaluator|XPathExpression|XPathResult|XSLTProcessor) interface/,
    ],
  },
  'idlharness.window.js': {
    comment:
      'Covers Window, Document and the node interfaces, which need a browser',
    omittedTests: true,
  },
} satisfies TestRunnerConfig;
