# Op-list schema v1 (omega_emit input)

You describe a program as one JSON object. `omega_emit` checks every op as it reads it and refuses
the first invalid one with a reason; a fully valid list becomes canonical OSC binary IR.

## Shape

```
{"functions": [ FUNCTION, ... ]}            1..32 functions
FUNCTION = {"name": NAME, "params": [TYPE, ...], "ret": TYPE or "void",
            "locals": [TYPE, ...], "blocks": [ BLOCK, ... ]}
BLOCK    = [ OP, ... ]                       1 or more ops; the LAST op is br, cbr or ret, and only the last
```

- Strict JSON: no comments, no trailing commas, no extra keys, every listed key present.
- NAME: `[A-Za-z_][A-Za-z0-9_]{0,62}`, unique in the unit.
- Registers are numbered: the params are registers 0..P-1 (in order), then the locals are
  registers P, P+1, ... (in order). At most 6 params and 480 registers in total.
- TYPE: `bool`, `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`, or an array reference
  `own:<elem>:<len>`, `shared:<elem>:<len>`, `mut:<elem>:<len>` (elem a scalar type, len 1..64).
  `ret` is a scalar type or `"void"`.
- Blocks are numbered from 0; block 0 is where the function starts. A function may call only
  functions listed before it (no recursion).
- Every register is read only after it has been written on every path that reaches the read
  (params count as written). Registers may be written more than once.

## Ops

Fields are register numbers unless stated. `dst` is written; the others are read.

| op | fields | meaning |
|---|---|---|
| `const` | `dst`, `value` | dst = value (a whole decimal number that fits dst's type; `true`/`false` for bool) |
| `mov` | `dst`, `a` | dst = a: the same scalar type; or, for an array dst, an array of the same element type and length: `own:` from `own:` (a move), `mut:` from `own:` or `mut:`, `shared:` from any |
| `add` `sub` `mul` `div` `rem` | `dst`, `a`, `b` | dst = a op b; dst, a, b the same integer type |
| `and` `or` `xor` | `dst`, `a`, `b` | bitwise (or logical, on bool); dst, a, b the same type |
| `shl` `shr` | `dst`, `a`, `b` | dst = a shifted by b; dst and a the same integer type, b any integer type |
| `neg` `bnot` | `dst`, `a` | dst = -a / bitwise not of a (same integer type) |
| `not` | `dst`, `a` | dst = logical not of a (bool) |
| `eq` `ne` `lt` `le` `gt` `ge` | `dst`, `a`, `b` | dst (bool) = a compared with b (a, b the same type) |
| `cast` | `dst`, `a` | dst = a converted to dst's integer type |
| `alloc` | `dst`, `init` | dst (an `own:` array) = new array, every element = init (element type) |
| `release` | `arr` | give an `own:` array back |
| `load` | `dst`, `arr`, `index` | dst = arr[index] (dst has the element type, index any integer type) |
| `store` | `arr`, `index`, `value` | arr[index] = value (arr is `own:` or `mut:`) |
| `call` | `func` (a function NAME), `args` (list of registers), `dst` (omit for a void callee) | dst = func(args) |
| `trap` | `code` (1..14) | stop with that trap code (the block still ends with br, cbr or ret) |
| `br` | `to` (block number) | continue at block `to` |
| `cbr` | `cond` (bool register), `then`, `else` (block numbers) | continue at `then` if cond, else at `else` |
| `ret` | `a` (omit in a void function) | return a (the function's ret type) |

Example: `fn clamp_sub(a: u32, b: u32) -> u32`: a - b when a >= b, else 0.

```
{"functions": [
  {"name": "clamp_sub", "params": ["u32", "u32"], "ret": "u32", "locals": ["bool", "u32", "u32"],
   "blocks": [
    [{"op": "ge", "dst": 2, "a": 0, "b": 1}, {"op": "cbr", "cond": 2, "then": 1, "else": 2}],
    [{"op": "sub", "dst": 3, "a": 0, "b": 1}, {"op": "ret", "a": 3}],
    [{"op": "const", "dst": 4, "value": 0}, {"op": "ret", "a": 4}]
   ]}
]}
```

A loop is a block that branches back to an earlier block (`br` or `cbr` to a lower block number).
