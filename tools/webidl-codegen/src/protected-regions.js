/**
 * Protected regions parser - preserves manual code sections during regeneration
 */

import fs from 'fs';

export class ProtectedRegions {
  constructor() {
    this.regions = new Map();
  }

  /**
   * Parse an existing file and extract protected regions
   */
  parseFile(filePath) {
    if (!fs.existsSync(filePath)) {
      return; // File doesn't exist yet, nothing to preserve
    }

    const content = fs.readFileSync(filePath, 'utf-8');
    this.parseContent(content);
  }

  /**
   * Parse content and extract protected regions
   */
  parseContent(content) {
    const lines = content.split('\n');
    let currentRegion = null;
    let regionContent = [];

    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];

      // Check for BEGIN marker
      const beginMatch = line.match(/\/\/\s*BEGIN\s+MANUAL\s+SECTION:\s*(.+)/);
      if (beginMatch) {
        currentRegion = beginMatch[1].trim();
        regionContent = [];
        continue;
      }

      // Check for END marker
      const endMatch = line.match(/\/\/\s*END\s+MANUAL\s+SECTION:\s*(.+)/);
      if (endMatch && currentRegion) {
        const regionName = endMatch[1].trim();
        if (regionName === currentRegion) {
          this.regions.set(currentRegion, regionContent.join('\n'));
          currentRegion = null;
          regionContent = [];
        }
        continue;
      }

      // Collect content within region
      if (currentRegion) {
        regionContent.push(line);
      }
    }
  }

  /**
   * Check if a region exists (has preserved content)
   */
  hasRegion(name) {
    return this.regions.has(name);
  }

  /**
   * Get preserved content for a region
   */
  getRegion(name) {
    return this.regions.get(name) || '';
  }

  /**
   * Generate a protected region block
   */
  generateRegion(name, defaultContent = '') {
    let code = '';
    code += `  // BEGIN MANUAL SECTION: ${name}\n`;

    if (this.hasRegion(name)) {
      // Use preserved content
      code += this.getRegion(name);
      if (!code.endsWith('\n')) {
        code += '\n';
      }
    } else {
      // Use default content
      if (defaultContent) {
        code += defaultContent;
        if (!code.endsWith('\n')) {
          code += '\n';
        }
      }
    }

    code += `  // END MANUAL SECTION: ${name}\n`;
    return code;
  }

  /**
   * Generate inline protected region (no indentation)
   */
  generateInlineRegion(name, defaultContent = '') {
    let code = '';
    code += `// BEGIN MANUAL SECTION: ${name}\n`;

    if (this.hasRegion(name)) {
      code += this.getRegion(name);
      if (!code.endsWith('\n')) {
        code += '\n';
      }
    } else {
      if (defaultContent) {
        code += defaultContent;
        if (!code.endsWith('\n')) {
          code += '\n';
        }
      }
    }

    code += `// END MANUAL SECTION: ${name}\n`;
    return code;
  }
}
