/**
 * ESLint rule to disallow re-exporting a namespace import as default.
 *
 * Disallows patterns like:
 *   import * as _default from 'some-module';
 *   export default _default;
 *
 * Instead, use:
 *   import { var1, var2 } from 'some-module';
 *   export * from 'some-module';
 *   export default { var1, var2 };
 *
 * @see https://github.com/cloudflare/workerd/issues/5844
 */
export const noExportDefaultOfImportStar = {
  meta: {
    type: 'problem',
    docs: {
      description:
        'Disallow exporting a namespace import as the default export',
    },
    messages: {
      noExportDefaultOfImportStar:
        "Do not re-export a namespace import as default. Use 'import { var1, var2 } from \"{{source}}\"', 'export * from \"{{source}}\"', and 'export default { var1, var2 }' instead.",
    },
    schema: [],
  },
  create(context) {
    const namespaceImports = new Map();

    return {
      ImportDeclaration(node) {
        // Look for: import * as X from '...'
        for (const specifier of node.specifiers) {
          if (specifier.type === 'ImportNamespaceSpecifier') {
            namespaceImports.set(specifier.local.name, node.source.value);
          }
        }
      },

      ExportDefaultDeclaration(node) {
        // Look for: export default X (where X is an identifier)
        if (node.declaration && node.declaration.type === 'Identifier') {
          const exportedName = node.declaration.name;
          const source = namespaceImports.get(exportedName);

          if (source !== undefined) {
            context.report({
              node,
              messageId: 'noExportDefaultOfImportStar',
              data: { source },
            });
          }
        }
      },
    };
  },
};

export default {
  rules: {
    'no-export-default-of-import-star': noExportDefaultOfImportStar,
  },
};
