// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { NextResponse } from 'next/server';

export async function GET(request) {
  const headers = {};
  request.headers.forEach((value, key) => {
    headers[key] = value;
  });

  const searchParams = {};
  request.nextUrl.searchParams.forEach((value, key) => {
    searchParams[key] = value;
  });

  return NextResponse.json({
    message: 'API response',
    timestamp: Date.now(),
    method: 'GET',
    headers,
    searchParams,
  });
}

export async function POST(request) {
  const body = await request.json().catch(() => ({}));

  return NextResponse.json({
    message: 'API response',
    timestamp: Date.now(),
    method: 'POST',
    received: body,
  });
}

export async function OPTIONS() {
  return new NextResponse(null, {
    status: 204,
    headers: {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
    },
  });
}
