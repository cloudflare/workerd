/**
 * Tests for custom ESLint rules.
 */
import { RuleTester } from 'eslint';
import { describe, it } from 'node:test';
import { noExportDefaultOfImportStar } from './custom-eslint-rules.mjs';

// Create a new RuleTester instance
const ruleTester = new RuleTester({
  languageOptions: {
    ecmaVersion: 2022,
    sourceType: 'module',
  },
});

describe('no-export-default-of-import-star', () => {
  it('should validate the rule', () => {
    ruleTester.run('no-export-default-of-import-star', noExportDefaultOfImportStar, {
      valid: [
        // Valid: Regular named import
        {
          code: `
            import { foo } from 'some-module';
            export default foo;
          `,
        },
        // Valid: Inline default export
        {
          code: `
            export default { foo: 'bar' };
          `,
        },
        // Valid: Export namespace import as named export
        {
          code: `
            import * as foo from 'some-module';
            export { foo };
          `,
        },
        // Valid: Namespace import used for something other than export default
        {
          code: `
            import * as foo from 'some-module';
            const bar = foo;
          `,
        },
        // Valid: Export default with non-identifier
        {
          code: `
            import * as foo from 'some-module';
            export default foo.bar;
          `,
        },
        // Valid: Re-export all
        {
          code: `
            export * from 'some-module';
          `,
        },
        // Valid: Named imports with object literal default export
        {
          code: `
            import { var1, var2 } from 'some-module';
            export * from 'some-module';
            export default { var1, var2 };
          `,
        },
      ],

      invalid: [
        // Invalid: Export namespace import as default
        {
          code: `
            import * as foo from 'some-module';
            export default foo;
          `,
          errors: [
            {
              messageId: 'noExportDefaultOfImportStar',
              data: { source: 'some-module' },
            },
          ],
        },
        // Invalid: Common pattern with _default
        {
          code: `
            import * as _default from 'some-module';
            export default _default;
          `,
          errors: [
            {
              messageId: 'noExportDefaultOfImportStar',
              data: { source: 'some-module' },
            },
          ],
        },
        // Invalid: Namespace import with scoped package
        {
          code: `
            import * as myModule from '@scope/package';
            export default myModule;
          `,
          errors: [
            {
              messageId: 'noExportDefaultOfImportStar',
              data: { source: '@scope/package' },
            },
          ],
        },
        // Invalid: Multiple imports, only one violates rule
        {
          code: `
            import { named } from 'module1';
            import * as ns from 'module2';
            export default ns;
          `,
          errors: [
            {
              messageId: 'noExportDefaultOfImportStar',
              data: { source: 'module2' },
            },
          ],
        },
      ],
    });
  });
});
