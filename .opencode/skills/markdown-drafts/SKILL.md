---
name: markdown-drafts
description: Use markdown formatting when drafting content intended for external systems (GitHub issues/PRs, Jira tickets, wiki pages, design docs, etc.) so formatting is preserved when the user copies it. Load this skill before producing any draft the user will paste elsewhere.
---

## Markdown Drafts

When the user asks you to draft, write, or compose content that will be copied into an external
system — GitHub issues, pull request descriptions, Jira tickets, wiki pages, design documents,
RFCs, or similar — **always use markdown syntax** so that formatting survives the copy-paste.

### When This Applies

- GitHub issues and pull request descriptions
- Jira ticket descriptions and comments
- Confluence / wiki page drafts
- Design documents and RFCs
- Slack messages (Slack renders a subset of markdown)
- Any content the user explicitly says will be copied elsewhere

### Formatting Rules

- Use `#`, `##`, `###` headers to create structure
- Use `-` or `*` for unordered lists, `1.` for ordered lists
- Use triple-backtick fenced code blocks with language tags (e.g., ` ```cpp `) for code
- Use `inline code` backticks for identifiers, file paths, commands, and config values
- Use `**bold**` for emphasis on key points and `_italic_` for secondary emphasis
- Use markdown tables when presenting structured comparisons or data
- Use `> blockquotes` for callouts, quoted text, or important notes
- Use `[link text](url)` for references — never bare URLs in running prose
- Use `---` horizontal rules to separate major sections when appropriate
- Use task lists (`- [ ]` / `- [x]`) when drafting action items or checklists
- Keep line lengths reasonable (e.g., 80-120 characters)

### Structure Guidelines

- Start with a concise summary paragraph before diving into details
- Use headers to break content into scannable sections
- Keep paragraphs short — walls of text are hard to scan in issue trackers
- Put the most important information first (inverted pyramid)
- End with next steps, open questions, or action items when relevant

### Rendering: Always Emit Raw Markdown

The chat interface renders markdown, which strips the raw syntax (`###`, `**`, `` ` ``, etc.) from
the output. Since these drafts are meant to be **copied and pasted** into external systems, the user
needs the raw markup characters preserved.

**Always wrap the entire draft in a fenced code block** so the chat interface displays it as literal
text. Use a plain triple-backtick fence (no language tag):

    ```
    ## My Heading

    - bullet one
    - **bold text** and `code`
    ```

This ensures the user sees and can copy the exact markdown source. Never render the draft as
formatted text outside a code fence — the markup will be silently consumed by the chat UI.

### What NOT To Do

- Do not render the draft as formatted markdown outside a code fence — the user will lose the syntax
- Do not use plain text formatting (e.g., ALL CAPS for headers, `====` underlines, manual indentation)
- Do not use HTML tags unless the target system requires them and markdown is insufficient
- Do not add emoji unless the user's draft style includes them or they explicitly request it
- Do not use common cliche AI-generated phrases, tropes, or filler content like tricolons,
  or generic intros/outros. Be concise and to the point.
- Do not include editorial comments or unsubstantiated claims. Stick to the facts and the user's
  instructions precisely. If you need to ask clarifying questions, do so instead of making assumptions.
