// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, doesNotThrow } from 'node:assert';

// Hyperdrive's connectionString must percent-encode the credentials and
// database so a structural character ('/', '?', '#', '%') in a component can't
// corrupt the URI.
function roundTrip(bindingName, user, password, database) {
  return {
    test(_ctrl, env) {
      const hd = env[bindingName];

      strictEqual(hd.user, user, 'discrete .user');
      strictEqual(hd.password, password, 'discrete .password');
      strictEqual(hd.database, database, 'discrete .database');

      const cs = hd.connectionString;
      doesNotThrow(() => new URL(cs), `not a valid URL: ${cs}`);
      const u = new URL(cs);
      strictEqual(decodeURIComponent(u.username), user, `username from ${cs}`);
      strictEqual(decodeURIComponent(u.password), password, `password from ${cs}`);
      strictEqual(
        decodeURIComponent(u.pathname.replace(/^\//, '')),
        database,
        `database from ${cs}`
      );
    },
  };
}

const APP = 'app_user';
const PW = 'plainpassword123';
const DB = 'neondb';

export const control = roundTrip('PLAIN', APP, PW, DB);

export const passwordSlash = roundTrip('PW_SLASH', APP, 'pa/ss', DB);
export const passwordQuestion = roundTrip('PW_QUESTION', APP, 'pa?ss', DB);
export const passwordHash = roundTrip('PW_HASH', APP, 'pa#ss', DB);
export const passwordAt = roundTrip('PW_AT', APP, 'pa@ss', DB);

// A literal '%XX' must be encoded as %25..., else `decodeURIComponent` reads it
// as an escape (e.g. '%2F' -> '/').
export const passwordPercent = roundTrip('PW_PERCENT', APP, 'pa%2Fss', DB);

export const userSlash = roundTrip('USER_SLASH', 'ap/p', PW, DB);
export const databaseQuestion = roundTrip('DB_QUESTION', APP, PW, 'ne?on');
