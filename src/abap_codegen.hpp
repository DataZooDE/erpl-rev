// Generating ABAP that carries user input.
//
// `erpl-adt object run` executes a parameterless class and nothing else -- no
// arguments, no stdin, no SUBMIT. So the only way for the CLI to pass a table
// name or a WHERE clause to SAP is to write it into ABAP source and deploy
// that. Which means every value the user typed becomes *code*.
//
// Treat this as injection-grade. A WHERE clause containing a quote must never
// be able to change the meaning of the generated program. The rules below are
// deliberately strict and deliberately fail loudly: refusing a value the user
// meant is recoverable, silently running something they did not mean is not.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace erpl_rev::abapgen {

// Thrown for anything that cannot be safely embedded. `field` names the CLI
// flag so the message can point at what the user typed.
struct UnsafeValue : std::runtime_error {
    UnsafeValue(const std::string &field, const std::string &why);
    std::string field;
};

inline constexpr size_t kMaxValueBytes = 4096;

// The maximum length of a generated source line. ADT accepts longer, the
// classic editor does not deal well with them, and a literal long enough to
// matter is split across `&&` continuations rather than emitted as one line.
inline constexpr size_t kMaxLineChars = 250;

// ---------------------------------------------------------------------------
// Literal emitters
//
// Each returns a complete ABAP *expression*, quotes included.
// ---------------------------------------------------------------------------

// ABAP text-string literal: `...`, with an embedded backtick doubled.
//
// This is the default for every user-supplied value, because it is the only
// form that preserves the value exactly. Contract: the returned expression
// evaluates at runtime to a `string` equal to `v` byte for byte, INCLUDING
// leading and trailing blanks.
//
// Long values are split into `` `a` && `b` `` so no source line exceeds
// kMaxLineChars; splits never land inside a multi-byte UTF-8 sequence.
std::string Backtick(const std::string &v, const std::string &field,
                     size_t max_bytes = kMaxValueBytes);

// ABAP character literal: '...', with an embedded apostrophe doubled.
//
// REFUSES a value with a trailing blank. The ABAP kernel strips trailing
// blanks from '...' literals, and SAP keys are blank-padded -- so
// `matnr = '000000000000012 '` would silently become a different predicate.
// Losing data quietly is worse than refusing, and Backtick is right there.
std::string Apostrophe(const std::string &v, const std::string &field,
                       size_t max_bytes = kMaxValueBytes);

// The body of an ABAP string template |...|: escapes \ | { } .
std::string TemplateBody(const std::string &v, const std::string &field,
                         size_t max_bytes = kMaxValueBytes);

std::string Int(long long v);
std::string Bool(bool v);

// A multi-line block (SQL, in practice) rendered as
//   |line one | &&
//   |line two|
// mirroring what Z_ERPL_REV_SQL's "Generate ABAP snippet" produces, trailing
// space on every line but the last so the statement reads correctly when the
// lines are concatenated. This is the one entry point that accepts newlines.
std::vector<std::string> MultilineTemplate(const std::string &text,
                                           const std::string &indent,
                                           const std::string &field);

// Is this a well-formed UTF-8 string with no control characters? Exposed for
// tests; the emitters apply it themselves.
bool IsEmbeddable(const std::string &v, std::string *why);

// ---------------------------------------------------------------------------
// Template engine
//
// Placeholders are `$ERPL_NAME$`, delimited on both sides so no key can be a
// prefix of another. Rendering is a single left-to-right pass: substituted
// text is never rescanned, so a value that happens to look like a placeholder
// is inert.
// ---------------------------------------------------------------------------

class Template {
public:
    explicit Template(std::string skeleton);

    // `escaped` must already be a complete ABAP expression -- the output of one
    // of the emitters above. Throws if the key appears nowhere in the skeleton,
    // which is the failure mode that makes a renamed placeholder silently stop
    // being substituted.
    Template &Set(const std::string &key, const std::string &escaped);

    // Throws if any `$ERPL_...$` remains unbound.
    std::string Render() const;

private:
    std::string skeleton_;
    std::vector<std::pair<std::string, std::string>> bound_;
};

// ---------------------------------------------------------------------------
// Result parsing
//
// Generated classes print `ERPL-CLI/<nonce> key=value` lines. The nonce is per
// invocation, so a stale class from an earlier run, or an error message that
// happens to echo a success string, cannot be mistaken for this run's result.
// ---------------------------------------------------------------------------

// 8 lowercase hex characters, from a non-deterministic source.
std::string MakeNonce();

// The value of `key` from the nonce-tagged line, or empty if that line is
// absent. Absence is never success.
std::string ResultField(const std::string &output, const std::string &nonce,
                        const std::string &key);

// Every `ERPL-CLI/<nonce>` line, in order, split into key/value pairs. One
// element per line; a line without '=' is skipped.
std::vector<std::pair<std::string, std::string>>
ResultLines(const std::string &output, const std::string &nonce);

} // namespace erpl_rev::abapgen
