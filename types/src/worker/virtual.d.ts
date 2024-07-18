declare module "virtual:comments.json" {
  const data: import("../transforms").CommentsData;
  export { data as default };
}

declare module "virtual:param-names.json" {
  const data: import("../generator/parameter-names").ParameterNamesData;
  export { data as default };
}
