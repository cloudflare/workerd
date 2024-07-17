declare module "workerd:rtti" {
  const api: {
    exportExperimentalTypes(): ArrayBuffer;
    exportTypes(compatDate: string, compatFlags: string[]): ArrayBuffer;
  };
  export { api as default };
}
