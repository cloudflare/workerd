interface PYODIDE_BUCKET {
  PYODIDE_PACKAGE_BUCKET_URL: string;
}

declare module "pyodide-internal:generated/pyodide-bucket.json" {
  const bucket: PYODIDE_BUCKET;
  export default bucket;
}
