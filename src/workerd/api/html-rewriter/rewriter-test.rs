// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests of the rewriter and token handles, driven from JavaScript as the facade drives them.

use jsg_test::Harness;

use crate::HtmlRewriterUtils;

/// `streams[id](sink)` writes the streaming content numbered `id`.
/// `rewrite(chunks, ons, docs, encoding)` rewrites the document written in `chunks` (Latin-1
/// strings) with the handlers of `on()` calls, `[selector, handlers]`, and of `onDocument()`
/// calls, `handlers`, and returns the output, from the dispatcher and the calls together. A
/// handler gets the token's handle and `endTags`, an array of end tag handlers that
/// `onEndTag(index)` refers to. `error(f)` returns the message of what `f` throws.
const PRELUDE: &str = r"
const enc = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0));
const dec = (b) => (b === undefined ? '' : String.fromCharCode(...b));
const names = ['element', 'comments', 'text', 'doctype', 'comments', 'text', 'end'];
const streams = [];
function newRewriter(ons, docs, encoding, sink) {
  const endTags = [];
  const element = ons.map(([selector, h]) => ({
    selector: utils.parseSelector(selector),
    element: !!h.element, comments: !!h.comments, text: !!h.text,
  }));
  const document = docs.map((h) => ({
    doctype: !!h.doctype, comments: !!h.comments, text: !!h.text, end: !!h.end,
  }));
  return utils.newRewriter(element, document, encoding ?? 'utf-8', (kind, id, token, output) => {
    sink(dec(output));
    if (kind === 7) return endTags[id](token);
    if (kind === 8) return streams[id](token);
    return (kind < 3 ? ons[id][1] : docs[id])[names[kind]](token, endTags);
  });
}
function rewrite(chunks, ons = [], docs = [], encoding) {
  let out = '';
  const rewriter = newRewriter(ons, docs, encoding, (s) => { out += s; });
  // The dispatcher appends to `out` during each call, so read it after the call.
  for (const chunk of [].concat(chunks)) {
    const written = dec(rewriter.write(enc(chunk)));
    out += written;
  }
  const ended = dec(rewriter.end());
  return out + ended;
}
function error(f) {
  try { f(); } catch (e) { return `${e.name}: ${e.message}`; }
  return 'no error';
}
";

fn run(code: &str) -> String {
    run_with(&Harness::new(), code)
}

fn run_with(harness: &Harness, code: &str) -> String {
    let mut result = String::new();
    harness.run_in_context(|lock, ctx| {
        let utils = jsg::resource::function_template_of::<HtmlRewriterUtils>(lock).unwrap();
        ctx.set_global(lock, "utils", utils);
        result = ctx.eval(lock, &format!("{PRELUDE}\n{code}")).unwrap();
        Ok(())
    });
    result
}

#[test]
fn passes_a_document_through_without_handlers() {
    assert_eq!(
        run("rewrite(['<!doctype html><p cl', 'ass=a>hi</p><!-- c -->'])"),
        "<!doctype html><p class=a>hi</p><!-- c -->"
    );
}

#[test]
fn element_handles_rewrite_the_element() {
    assert_eq!(
        run(r"rewrite('<div><p a=1>x</p></div>', [['p', { element(el) {
            el.setAttribute('b', '2');
            el.removeAttribute('a');
            el.before('<i>', false);
            el.after('<i>', true);
            el.prepend('[', false);
            el.append(']', false);
            el.setTagName('span');
        } }]])"),
        "<div>&lt;i&gt;<span b=\"2\">[x]</span><i></div>"
    );
    assert_eq!(
        run(r"rewrite('<p>a</p><p>b</p><p>c</p>', [['p', { element(el) {
            const text = el.getAttribute('id');
            if (el.hasAttribute('id')) throw new Error('unexpected ' + text);
            ({ 0: () => el.remove(), 1: () => el.removeAndKeepContent(),
               2: () => el.setInnerContent('<b>', true) })[globalThis.n = (globalThis.n ?? -1) + 1]();
        } }]])"),
        "b<p><b></p>"
    );
    assert_eq!(
        run(r"let seen = [];
            rewrite('<svg><g></g></svg><div></div>', [['*', { element(el) {
              seen.push(`${el.tagName()} ${el.namespaceUri()} ${el.removed()}`);
              el.remove();
              seen.push(el.removed());
            } }]]);
            seen.join(',')"),
        "svg http://www.w3.org/2000/svg false,true,g http://www.w3.org/2000/svg true,true,\
         div http://www.w3.org/1999/xhtml false,true"
    );
}

#[test]
fn element_handles_list_attributes() {
    assert_eq!(
        run(r"let seen;
            rewrite('<p A=1 b>', [['p', { element(el) {
              seen = [el.attributeCount(), el.attribute(0), el.attribute(1), el.attribute(2)];
            } }]]);
            JSON.stringify(seen)"),
        r#"[2,["a","1"],["b",""],null]"#
    );
}

#[test]
fn handlers_get_their_kind_id_and_token() {
    assert_eq!(
        run(r"let seen = [];
            const log = (name) => (token) => { seen.push(name); };
            rewrite('<!doctype html><!--x--><p>t</p>', [
              ['p', { element: log('el0'), text: (t) => { seen.push(`text0 ${t.text()} ${t.lastInTextNode()}`); } }],
              ['p', { element: log('el1') }],
            ], [{
              doctype: (d) => { seen.push(`doctype ${d.name()} ${d.publicId()} ${d.systemId()}`); },
              comments: (c) => { seen.push(`comment ${c.text()} ${c.removed()}`); },
              end: (e) => { e.append('<!--end-->', true); seen.push('end'); },
            }]);
            seen.join(',')"),
        "doctype html undefined undefined,comment x false,el0,el1,text0 t false,text0  true,end"
    );
}

#[test]
fn text_and_comment_handles_rewrite_their_token() {
    assert_eq!(
        run(r"rewrite('<p>ab<!--c--></p>', [['p', {
              text(t) { if (t.text()) { t.before('<', false); t.after('>', true); t.replace('x', false); } },
              comments(c) { c.setText('d'); c.before('1', false); c.after('2', false); },
            }]])"),
        "<p>&lt;x>1<!--d-->2</p>"
    );
    assert_eq!(
        run(r"rewrite('<p>ab<!--c--></p>', [['p', {
              text(t) { t.remove(); }, comments(c) { c.replace('r', false); },
            }]])"),
        "<p>r</p>"
    );
    assert_eq!(
        run(r"let message;
            rewrite('<!--c-->', [], [{ comments(c) { message = error(() => c.setText('-->')); } }]);
            message"),
        "TypeError: Parser error: Comment text shouldn't contain a comment-closing sequence."
    );
}

#[test]
fn end_tag_handlers_run_for_the_element_end_tag() {
    assert_eq!(
        run(
            r"rewrite('<p>x</p><p>y</p>', [['p', { element(el, endTags) {
              endTags.push((t) => { t.before('!', false); t.setName('div'); });
              el.onEndTag(endTags.length - 1);
              // A second registration replaces the first.
              endTags.push((t) => { t.after(t.name(), false); });
              el.onEndTag(endTags.length - 1);
            } }]])"
        ),
        "<p>x</p>p<p>y</p>p"
    );
    assert_eq!(
        run(r"let message;
            rewrite('<br>', [['br', { element(el) { message = error(() => el.onEndTag(0)); } }]]);
            message"),
        "TypeError: Parser error: No end tag."
    );
}

#[test]
fn the_dispatcher_gets_the_output_before_each_handler() {
    assert_eq!(
        run(r"let parts = [];
            const rewriter = newRewriter([['p', { element() {} }]], [], 'utf-8', (s) => parts.push(s));
            parts.push('|' + dec(rewriter.write(enc('<a></a><p>x'))));
            parts.push('|' + dec(rewriter.end()));
            JSON.stringify(parts)"),
        r#"["<a></a>","|<p>x","|"]"#
    );
}

#[test]
fn tokens_are_invalid_after_their_handler() {
    assert_eq!(
        run(r"let kept;
            rewrite('<p>', [['p', { element(el) { kept = el; } }]]);
            error(() => kept.tagName())"),
        "TypeError: This content token is no longer valid. Content tokens are only valid during \
         the execution of the relevant content handler."
    );
}

#[test]
fn a_throwing_handler_fails_the_rewriter_with_its_exception() {
    assert_eq!(
        run(r"const thrown = new RangeError('boom');
            let caught;
            const rewriter = newRewriter([['p', { element() { throw thrown; } }]], [], 'utf-8', () => {});
            try { rewriter.write(enc('<p>')); } catch (e) { caught = e; }
            [caught === thrown, error(() => rewriter.write(enc('x'))), error(() => rewriter.end())].join()"),
        "true,TypeError: This HTMLRewriter has failed.,TypeError: This HTMLRewriter has failed."
    );
}

#[test]
fn a_handler_returning_a_promise_fails_the_rewriter() {
    assert_eq!(
        run(r"error(() => rewrite('<p>', [['p', { element: async () => {} }]]))"),
        "TypeError: HTMLRewriter handlers that return a promise are not supported yet."
    );
}

#[test]
fn a_handler_can_not_use_its_own_rewriter() {
    assert_eq!(
        run(r"let messages = [];
            const rewriter = newRewriter([['p', { element() {
              messages.push(error(() => rewriter.write(enc('x'))));
              messages.push(error(() => rewriter.end()));
              messages.push(error(() => rewriter.abort()));
            } }]], [], 'utf-8', () => {});
            rewriter.write(enc('<p>'));
            messages.push(dec(rewriter.end()));
            messages.join('\n')"),
        "TypeError: An HTMLRewriter handler can't use the rewriter that is running it.\n\
         TypeError: An HTMLRewriter handler can't use the rewriter that is running it.\n\
         TypeError: An HTMLRewriter handler can't use the rewriter that is running it.\n"
    );
}

#[test]
fn ended_and_aborted_rewriters_fail() {
    assert_eq!(
        run(r"const ended = newRewriter([], [], 'utf-8', () => {});
            ended.end();
            const aborted = newRewriter([], [], 'utf-8', () => {});
            aborted.abort();
            [error(() => ended.write(enc('x'))), error(() => ended.end()),
             error(() => aborted.write(enc('x')))].join('\n')"),
        "TypeError: This HTMLRewriter has already ended.\n\
         TypeError: This HTMLRewriter has already ended.\n\
         TypeError: This HTMLRewriter has failed."
    );
}

#[test]
fn invalid_input_fails_with_parser_errors() {
    assert_eq!(
        run(r"[error(() => utils.parseSelector('p:hover')),
             error(() => rewrite('', [], [], 'no-such-encoding')),
             error(() => rewrite('', [], [], 'utf-16le')),
             error(() => rewrite('<p>', [['p', { element: (el) => el.setTagName('') }]])),
             error(() => rewrite('<p>', [['p', { element: (el) => el.setAttribute('a b', '') }]])),
            ].join('\n')"),
        "TypeError: Parser error: Unsupported pseudo-class or pseudo-element in selector.\n\
         TypeError: Parser error: Unknown character encoding has been provided.\n\
         TypeError: Parser error: Expected ASCII-compatible encoding.\n\
         TypeError: Parser error: Tag name can't be empty.\n\
         TypeError: Parser error: ` ` character is forbidden in the attribute name"
    );
}

#[test]
fn the_document_encoding_applies_to_input_and_output() {
    // "é" is 0xE9 in windows-1252. `after()` runs for both chunks of the text node, including the
    // empty last one.
    assert_eq!(
        run(r"let text;
            const out = rewrite('<p>\xe9</p>', [['p', { text(t) { text ??= t.text(); t.after('\xe9', false); } }]],
                                [], 'windows-1252');
            JSON.stringify([text, out])"),
        r#"["é","<p>ééé</p>"]"#
    );
}

#[test]
fn the_esi_compatibility_flag_makes_esi_include_a_void_tag() {
    let code = r"let message;
        const out = rewrite('<esi:include src=a>x</esi:include>', [['*', { element(el, endTags) {
          endTags.push((t) => { t.before('!', false); });
          message = error(() => el.onEndTag(endTags.length - 1));
        } }]]);
        `${message} ${out}`";
    assert_eq!(run(code), "no error <esi:include src=a>x!</esi:include>");
    let harness = Harness::with_flags(|mut flags| flags.set_esi_include_is_void_tag(true));
    assert_eq!(
        run_with(&harness, code),
        "TypeError: Parser error: No end tag. <esi:include src=a>x</esi:include>"
    );
}

#[test]
fn streaming_content_is_written_where_the_token_inserted_it() {
    let streams = r"for (const s of ['[b]', '[p]', '[ap]', '[a]', '[i]', '[r]', '[eb]', '[ea]',
                                     '[tb]', '[tr]', '[ta]']) {
                      streams.push((sink) => { sink.write(enc(s)); });
                    }";
    assert_eq!(
        run(&format!(
            r"{streams}
            rewrite('<p>x</p>', [['p', {{ element(el) {{
              el.beforeStream(0, true);
              el.prependStream(1, true);
              el.appendStream(2, true);
              el.afterStream(3, true);
            }} }}]])"
        )),
        "[b]<p>[p]x[ap]</p>[a]"
    );
    assert_eq!(
        run(&format!(
            r"{streams}
            rewrite('<p>x</p><div>y</div>', [
              ['p', {{ element(el) {{ el.setInnerContentStream(4, true); }} }}],
              ['div', {{ element(el) {{ el.replaceStream(5, true); }} }}],
            ])"
        )),
        "<p>[i]</p>[r]"
    );
    assert_eq!(
        run(&format!(
            r"{streams}
            rewrite('<p>x</p>', [['p', {{ element(el, endTags) {{
              endTags.push((t) => {{ t.beforeStream(6, true); t.afterStream(7, true); }});
              el.onEndTag(endTags.length - 1);
            }} }}]])"
        )),
        "<p>x[eb]</p>[ea]"
    );
    assert_eq!(
        run(&format!(
            r"{streams}
            rewrite('<p>x</p>', [['p', {{ text(t) {{
              if (t.text()) {{ t.beforeStream(8, true); t.replaceStream(9, true); t.afterStream(10, true); }}
            }} }}]])"
        )),
        "<p>[tb][tr][ta]</p>"
    );
}

#[test]
fn streaming_content_is_utf_8_text_or_html() {
    // "é" is 0xC3 0xA9 in UTF-8, written in two chunks; the output is UTF-8 too.
    assert_eq!(
        run(r"streams.push((sink) => {
                sink.write(enc('<b>'));
                sink.write(Uint8Array.of(0xc3));
                sink.write(Uint8Array.of(0xa9));
              });
              rewrite('<p>x</p><i>y</i>', [
                ['p', { element(el) { el.replaceStream(0, true); } }],
                ['i', { element(el) { el.replaceStream(0, false); } }],
              ])"),
        "<b>\u{c3}\u{a9}&lt;b&gt;\u{c3}\u{a9}"
    );
}

#[test]
fn invalid_streaming_content_fails_the_rewriter() {
    assert_eq!(
        run(r"let kept;
              streams.push((sink) => { kept = sink; sink.write(Uint8Array.of(0xff)); });
              [error(() => rewrite('<p>', [['p', { element(el) { el.replaceStream(0, true); } }]])),
               error(() => kept.write(enc('x')))].join('\n')"),
        "TypeError: Parser error: Invalid UTF-8\n\
         TypeError: This content token is no longer valid. Content tokens are only valid during \
         the execution of the relevant content handler."
    );
}
