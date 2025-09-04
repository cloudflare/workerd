// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'buffered-flag-after-timeout.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      'PerformanceObserver with buffered flag sees entry after timeout',
    ],
  },
  'buffered-flag-observer.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      'PerformanceObserver with buffered flag should see past and future entries.',
    ],
  },
  'buffered-flag-with-entryTypes-observer.tentative.any.js': {
    comment: 'This is a tentative test',
    omittedTests: true,
  },
  'case-sensitivity.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      'getEntriesByType values are case sensitive',
      'getEntriesByName values are case sensitive',
      'observe() and case sensitivity for types/entryTypes and buffered.',
    ],
  },
  'droppedentriescount.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      'Dropped entries count is 0 when there are no dropped entries of relevant type.',
      'Dropped entries correctly counted with multiple types.',
      'Dropped entries counted even if observer was not registered at the time.',
      'Dropped entries only surfaced on the first callback.',
      'Dropped entries surfaced after an observe() call!',
    ],
  },
  'idlharness.any.js': {
    comment:
      'Test file /resources/WebIDLParser.js not found. Update wpt_test.bzl to handle this case.',
    omittedTests: true,
  },
  'multiple-buffered-flag-observers.any.js': {
    comment: 'This is not yet implemented',
    omittedTests: [
      'Multiple PerformanceObservers with buffered flag see all entries',
    ],
  },
  'navigation-id.helper.js': {},
  'not-restored-reasons/abort-block-bfcache.window.js': {
    comment: 'This is not yet implemented',
    omittedTests: ['aborting a parser should block bfcache.'],
  },
  'not-restored-reasons/performance-navigation-timing-attributes.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/performance-navigation-timing-bfcache-reasons-stay.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/performance-navigation-timing-bfcache.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/performance-navigation-timing-cross-origin-bfcache.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/performance-navigation-timing-fetch.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/performance-navigation-timing-iframes-without-attributes.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/performance-navigation-timing-lock.https.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/performance-navigation-timing-navigation-failure.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/performance-navigation-timing-not-bfcached.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/performance-navigation-timing-redirect-on-history.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/performance-navigation-timing-reload.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/performance-navigation-timing-same-origin-bfcache.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/performance-navigation-timing-same-origin-replace.tentative.window.js':
    {
      comment: 'This is a tentative test',
      omittedTests: true,
    },
  'not-restored-reasons/test-helper.js': {
    comment:
      'Test file /html/browsers/browsing-the-web/back-forward-cache/resources/rc-helper.js not found. Update wpt_test.bzl to handle this case.',
    omittedTests: true,
  },
  'observer-buffered-false.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      'PerformanceObserver without buffered flag set to false cannot see past entries.',
    ],
  },
  'performanceentry-tojson.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: ['Test toJSON() in PerformanceEntry'],
  },
  'performanceobservers.js': {},
  'po-callback-mutate.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      'PerformanceObserver modifications inside callback should update filtering and not clear buffer',
    ],
  },
  'po-disconnect-removes-observed-types.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      'Types observed are forgotten when disconnect() is called.',
    ],
  },
  'po-disconnect.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      'disconnected callbacks must not be invoked',
      'disconnecting an unconnected observer is a no-op',
      'An observer disconnected after a mark must not have its callback invoked',
    ],
  },
  'po-entries-sort.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      'getEntries, getEntriesByType, getEntriesByName sort order',
    ],
  },
  'po-getentries.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: ['getEntries, getEntriesByType and getEntriesByName work'],
  },
  'po-mark-measure.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      'entries are observable',
      'mark entries are observable',
      'measure entries are observable',
    ],
  },
  'po-observe-repeated-type.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      "Two calls of observe() with the same 'type' cause override.",
    ],
  },
  'po-observe-type.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      "Calling observe() without 'type' or 'entryTypes' throws a TypeError",
      'Calling observe() with entryTypes and then type should throw an InvalidModificationError',
      'Calling observe() with type and then entryTypes should throw an InvalidModificationError',
      'Calling observe() with type and entryTypes should throw a TypeError',
      'Passing in unknown values to type does throw an exception.',
      'observe() with different type values stacks.',
    ],
  },
  'po-observe.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: [
      'entryTypes must be a sequence or throw a TypeError',
      'Empty sequence entryTypes does not throw an exception.',
      'Unknown entryTypes do not throw an exception.',
      'Filter unsupported entryType entryType names within the entryTypes sequence',
      'Check observer callback parameter and this values',
      'replace observer if already present',
    ],
  },
  'po-takeRecords.any.js': {
    comment: 'This is not yet implemented',
    disabledTests: ["Test PerformanceObserver's takeRecords()"],
  },
  'supportedEntryTypes.any.js': {
    comment:
      'Test expect non-empty array that is cached. Our implementation does not cache it yet.',
    disabledTests: [
      'supportedEntryTypes exists and returns entries in alphabetical order',
      'supportedEntryTypes caches result',
    ],
  },
  'webtiming-resolution.any.js': {
    comment: 'We intentionally fail on these',
    disabledTests: [
      'Verifies the resolution of performance.now() is at least 5 microseconds.',
      'Verifies the resolution of entry.startTime is at least 5 microseconds.',
    ],
  },
} satisfies TestRunnerConfig;
