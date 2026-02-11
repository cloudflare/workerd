// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { Suspense } from 'react';
import { cookies } from 'next/headers';

async function AsyncSection({ id }) {
  return <div id={`async-section-${id}`}>Loaded data for section-{id}</div>;
}

export default async function Home() {
  const cookieStore = await cookies();
  const testCookie = cookieStore.get('test-cookie');

  return (
    <main>
      <h1>SSR Test Page</h1>
      <p id="server-time">Server rendered at: {Date.now()}</p>
      <p id="cookie-value">Cookie value: {testCookie?.value ?? 'not set'}</p>
      <section id="suspense-section">
        <h2>Suspense Boundaries</h2>
        <Suspense fallback={<div>Loading 1...</div>}>
          <AsyncSection id="1" />
        </Suspense>
        <Suspense fallback={<div>Loading 2...</div>}>
          <AsyncSection id="2" />
        </Suspense>
        <Suspense fallback={<div>Loading 3...</div>}>
          <AsyncSection id="3" />
        </Suspense>
      </section>
    </main>
  );
}
