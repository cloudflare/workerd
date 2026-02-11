// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default function StreamingPage() {
  return (
    <main>
      <h1>Streaming Test Page</h1>
      <div id="large-content">
        {Array.from({ length: 100 }, (_, i) => (
          <p key={i}>Content chunk {i + 1}</p>
        ))}
      </div>
    </main>
  );
}
