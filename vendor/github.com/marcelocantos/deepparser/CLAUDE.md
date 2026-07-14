# deepparser

SQL parser for SQLite with sqldeep grammar extensions. Forked from
[sqliteai/liteparser](https://github.com/sqliteai/liteparser); same
Lemon LALR(1) grammar (`src/lp_parse.y`), same tokenizer
(`src/lp_tokenize.c`), same AST and arena model — extended to
recognise sqldeep's JSON-shaped SELECT projections, join arrows,
JSON-path operator, XML literals, and recursive selects.

Used by [sqldeep](https://github.com/marcelocantos/sqldeep) as its
front-line SQL parser. Standalone consumers welcome but not the
primary use case.

## Build

```sh
make            # build static + dynamic libraries + sqlparse CLI
make test       # build and run all tests (currently upstream tests only)
make clean
```

Lemon parser generator is built locally from `lemon.c` (vendored).
`make grammar` regenerates `parse.c` from `src/lp_parse.y`.

## Upstream tracking

- `origin` → `github.com/marcelocantos/deepparser` (our fork).
- `upstream` → `github.com/sqliteai/liteparser` (read-only).
- Pull upstream changes via `git fetch upstream && git merge upstream/main`
  when meaningful commits land. Upstream churn is low (~5 commits over
  ~6 months) and almost entirely additive.

## Architecture

Three concerns, three files:

1. **`lp_tokenize.c`** — hand-written tokenizer. Adapted from SQLite's
   `tokenize.c`. Emits a stream of `LpToken` with source positions.
2. **`lp_parse.y`** — Lemon grammar. Compiled to `parse.c`. Builds
   `LpNode` AST via arena-allocated nodes.
3. **`lp_unparse.c`** — AST → SQL text. Canonical (not byte-identical to
   input). New sqldeep node kinds get matching emitters here.

## Sqldeep extensions

Added on top of the standard SQLite grammar:

- **Object literal** `{ field, key: expr, ... }` — expression.
  Trailing commas allowed (`{a, b,}`).
- **Qualified bare field** `{ sm.repo, s.t.col }` — key is the last
  dotted component; value is the full column-ref.
- **Bare SELECT / FROM-first as field value** `{ field: SELECT ... }`
  and `{ field: FROM t WHERE ... SELECT ... }` — wrapped internally
  in an `LP_EXPR_SUBQUERY`. The user can also write `(SELECT ...)`
  explicitly; both produce the same AST.
- **Array literal** `[ expr, ... ]` — expression. Trailing commas allowed.
- **Deep SELECT projection** `SELECT { ... }` / `SELECT [ ... ]` /
  `SELECT/1 { ... }` — statement / subquery.
- **FROM-first variant** `FROM ... [WHERE ...] [GROUP BY ...]
  [HAVING ...] [ORDER BY ...] [LIMIT ...] SELECT { ... }` —
  alternative ordering with all filter/sort/limit clauses placed
  *before* SELECT. The projection terminates the statement; no
  trailing clauses after SELECT in this form.
- **Join arrow** `c->orders o ON|USING ...`, `<-` for reverse, chains
  and bridges — in FROM-position only.
- **JSON path** `(expr).field.sub[n]`, including bracket-first
  `(expr)[0]` and any interleaving of `.name` / `[N]` segments.
- **XML element** `<tag attr="v">body</tag>`, self-closing, namespaced,
  with `{expr}` interpolation. `jsx(...)` and `jsonml(...)` wrappers
  select alternative emit modes (sqldeep concern, see node `mode`
  field). Inside attribute and identifier tokens, `\<char>` escape
  is recognised in double-quoted strings (`{"say \"hi\"": v}`).
- **Recursive SELECT** `SELECT/1 { ..., children: * } FROM t RECURSE ON (fk [= pk]) WHERE ...`.
- **`SELECT/1`** singular modifier.

Grammar productions for these live in `src/lp_parse.y` under
sqldeep-tagged sections. Corresponding `LpNodeKind`s have an
`LP_SQLDEEP_` prefix.

### Field-comma tokenization (the LBRACE/LBRACKET stop-at-comma rule)

Inside a sqldeep `{...}` or `[...]` literal, any COMMA at the
outermost level of that brace frame is promoted by the tokenizer
driver to `FIELD_COMMA` — the field/element separator the grammar
expects. This lets a bare SELECT field value terminate at the
correct point even when its own clauses contain commas:

```
{ a: SELECT { x } FROM t1, b: SELECT { y } FROM t2 }
                         ^^^ FIELD_COMMA, ends a's field value
```

Implication: inside a bare SELECT field value, you cannot use
comma-join in FROM (`FROM t1, t2`) or multi-item ORDER BY /
GROUP BY (`ORDER BY a, b`) — the COMMA terminates the field.
Either use explicit JOIN keywords or wrap the inner SELECT in
parens (`{ field: (SELECT ... FROM t1, t2 ORDER BY a, b) }`).

This matches sqldeep's hand-written parser behaviour.

Token disambiguation:

- `->` followed by ident → `JOIN_ARROW`; followed by string/expr →
  binary `LP_OP_PTR`.
- `<` followed by ident-start → `XML_LT`, **unless** the previous token
  ended an expression (TK_ID, INTEGER, RP, RBRACE, etc.). Exception:
  the sqldeep singular modifier `SELECT/1` ends with INTEGER but a
  result-column expression follows, so the `SLASH INTEGER` prefix is
  whitelisted for XML promotion. This means `WHERE n < a` still parses
  as binary `LT`; `SELECT <div>x</div>` parses as XML.
- `/` after `SELECT` → `SLASH_ONE` if followed by `1`; otherwise
  divide.
- `[` is sqldeep array start (SQLite bracketed-identifier lexing
  disabled — non-portable, not a sqldeep target).
- COMMA at the top level of a sqldeep `{...}` or `[...]` literal
  is emitted as `FIELD_COMMA`. The grammar uses `FIELD_COMMA` as
  the object/array field separator; regular `COMMA` continues to
  separate items inside parens, SELECT-result lists, etc.
- XML body context is tracked via an `LpXmlFrame` stack on the parse
  context. In `BODY` phase the tokenizer emits `XML_TEXT` for raw text
  up to the next `<` or `{`. `</` → `XML_END_LT`. In `TAG_OPEN` /
  `TAG_CLOSE` phase `>` → `XML_GT`, `/>` → `XML_SLASH_GT`, `:` →
  `COLON` (namespace separator, not `:name` variable), and any
  alpha-start token is forced to `TK_ID` so that SQL keywords like
  `TABLE` can appear as tag or attribute names. `{` inside `BODY`
  pushes an `INTERP` frame; the tokenizer tracks brace depth so
  nested object literals (`{{ ... }}`) close correctly.

## Round-trip

Parse → AST → unparse → semantically equivalent text, at parity with
upstream liteparser. **Not byte-identical**: whitespace normalised,
comma-joins expanded to `INNER JOIN`, `i+1` → `i + 1`, etc. This
matches upstream behaviour and is not a regression.

## File layout

```
src/
  arena.[ch]              Arena allocator
  liteparser.[ch]         Public API, AST types, visitor, JSON
  liteparser_internal.h   Internal types
  lp_lempar.c             Lemon parser template (generated)
  lp_parse.y              Grammar source
  lp_tokenize.c           Hand-written tokenizer
  lp_unparse.c            AST → SQL
  parse.[ch]              Lemon-generated parser
test/                     Test suite (upstream + sqldeep additions)
fuzz/                     Fuzz harness
wasm/                     WASM build
Makefile                  Build
```

## Licensing

MIT License throughout, matching upstream. Modifications to upstream
files inherit MIT; new files added in this fork are also MIT. The
LICENSE file carries both the upstream SQLite AI copyright and a
contributor line for sqldeep grammar extensions. See `LICENSE`.

Note: this deviates from the global "always Apache 2.0" convention.
Relicensing upstream MIT code is not an option, and dual-licensing
just the additions would create a per-file licensing minefield that
doesn't pay for itself. Sqldeep itself (the consumer) remains
Apache 2.0; the MIT/Apache 2.0 boundary is at the submodule edge.
