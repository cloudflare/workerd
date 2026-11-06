'use strict';

// The legacy (url_original) backend: a URL is a LegacyUrlHandle
// (api/url.h), which holds a kj::Url and implements the legacy parser's
// attributes, setters and query operations unchanged. A URL's handle is
// also its searchParams' store, so the two share one kj::Url and need no
// synchronization.

import type { UrlAttributes, UrlBackend } from './backend';
import type { InternalArrayHelpers } from './internal-array';

const { ArrayPrototypePush } = primordials;

const { createLegacySearchParams, createLegacyUrl, tryCreateLegacyUrl } = utils;

const {
  HREF,
  PROTOCOL,
  USERNAME,
  PASSWORD,
  HOST,
  HOSTNAME,
  PORT,
  PATHNAME,
  SEARCH,
  HASH,
} = require('url/backend') as UrlAttributes;
const { internalArray } = require('url/internal-array') as InternalArrayHelpers;

const backend: UrlBackend<LegacyUrlHandle> = {
  parse(url, base) {
    return createLegacyUrl(url, base);
  },

  tryParse(url, base) {
    return tryCreateLegacyUrl(url, base);
  },

  canParse(url, base) {
    return tryCreateLegacyUrl(url, base) !== undefined;
  },

  get(url, attribute) {
    switch (attribute) {
      case HREF:
        return url.getHref();
      case PROTOCOL:
        return url.getProtocol();
      case USERNAME:
        return url.getUsername();
      case PASSWORD:
        return url.getPassword();
      case HOST:
        return url.getHost();
      case HOSTNAME:
        return url.getHostname();
      case PORT:
        return url.getPort();
      case PATHNAME:
        return url.getPathname();
      case SEARCH:
        return url.getSearch();
      case HASH:
        return url.getHash();
    }
  },

  set(url, attribute, value) {
    switch (attribute) {
      case HREF:
        return url.setHref(value);
      case PROTOCOL:
        url.setProtocol(value);
        return true;
      case USERNAME:
        url.setUsername(value);
        return true;
      case PASSWORD:
        url.setPassword(value);
        return true;
      case HOST:
        url.setHost(value);
        return true;
      case HOSTNAME:
        url.setHostname(value);
        return true;
      case PORT:
        url.setPort(value);
        return true;
      case PATHNAME:
        url.setPathname(value);
        return true;
      case SEARCH:
        url.setSearch(value);
        return true;
      case HASH:
        url.setHash(value);
        return true;
    }
  },

  origin(url) {
    return url.getOrigin();
  },

  storeForUrl(url) {
    return url;
  },

  newStore(init) {
    const store = createLegacySearchParams();
    if (typeof init === 'string') {
      // assignQueryString() removes the leading '?' itself.
      if (init.length > 0) store.assignQueryString(init);
      return store;
    }
    const pairs = internalArray<string>();
    for (let i = 0; i < init.length; i++) {
      const entry = init[i] as [string, string];
      ArrayPrototypePush(pairs, entry[0], entry[1]);
    }
    store.assignQueryPairs(pairs);
    return store;
  },
};

module.exports = backend;
