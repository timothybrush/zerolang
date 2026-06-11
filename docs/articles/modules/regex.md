## When To Use std.regex

In Zerolang, use `std.regex` to match text against a documented ECMA-262-leaning
regular expression subset, such as JSON Schema `pattern` checks.

This module is graph-backed. The compiler uses its standard-library graph store,
while the projection snippets below show the human-readable projection that agents may
export for review. Agents should discover helpers with `zero skills get stdlib`,
inspect user packages with `zero query [graph-input]` or
`zero inspect [graph-input]`, and patch user code through the graph instead of
hand-editing `.0` files.

Supported syntax: literals, `.`, character classes with negation, ranges, and
`\d \D \w \W \s \S`, anchors `^` `$` and word boundaries `\b` `\B`, greedy
quantifiers `*` `+` `?` `{m}` `{m,}` `{m,n}`, alternation `|`, and capturing or
`(?:...)` non-capturing groups (matching only; no capture extraction). Matching
is by Unicode codepoint over UTF-8 text and searches anywhere in the text unless
the pattern is anchored, like ECMAScript `RegExp.prototype.test`.

Unsupported constructs never misparse silently. Compilation fails with a
structured status code: `1` backreference, `2` lookahead, `3` lookbehind,
`4` named group, `5` lazy quantifier, `6` group modifier or inline flags,
`7` unicode property escape, `8` invalid syntax, `9` invalid quantifier range,
`10` program over the buffer or 2048-byte limit, `11` pattern is not valid
UTF-8, `12` group nesting over depth 32.

Runnable today:

| API | Return | Notes |
| --- | --- | --- |
| `std.regex.compile(buffer, pattern)` | `Maybe<Span<u8>>` | Compiles a pattern into a caller-owned buffer; returns the compiled program span or `null` on any compile failure. |
| `std.regex.compileStatus(buffer, pattern)` | `u32` | Compiles and returns `0` or the structured status code for diagnostics. |
| `std.regex.statusName(status)` | `String` | Names a status code, such as `unsupported backreference`. |
| `std.regex.isMatch(program, text)` | `Bool` | Tests text against a compiled program. Compile once, then match many times. |
| `std.regex.matches(pattern, text)` | `Maybe<Bool>` | One-shot compile and match with an internal 1024-byte program buffer; returns `null` when the pattern does not compile. |

## Example

```zero
pub fn main(world: World) -> Void raises {
    var storage: [512]u8 = [0; 512]
    let buffer: MutSpan<u8> = storage
    let compiled: Maybe<Span<u8>> = std.regex.compile(buffer, "^[a-z]+-\\d{2,4}$")
    if !compiled.has {
        return
    }
    let program: Span<u8> = compiled.value
    let quick: Maybe<Bool> = std.regex.matches("^(cat|dog)s?$", "dogs")
    if std.regex.isMatch(program, "build-2048") && !std.regex.isMatch(program, "build-1") && (quick.has && quick.value) {
        check world.out.write("regex ok\n")
    }
}
```

Diagnosing a rejected pattern:

```zero
pub fn main(world: World) -> Void raises {
    var storage: [128]u8 = [0; 128]
    let buffer: MutSpan<u8> = storage
    let status: u32 = std.regex.compileStatus(buffer, "(?=lookahead)")
    if status != 0 {
        check world.out.write(std.regex.statusName(status))
        check world.out.write("\n")
    }
}
```

Effects: none.

Allocation behavior: `compile` and `compileStatus` write the caller buffer;
`isMatch` and `matches` allocate nothing on the heap.

Error behavior: `compile` returns `null` and `compileStatus` returns a status
code naming the construct; `matches` returns `null` for invalid patterns;
`isMatch` returns `false` for malformed program spans or invalid UTF-8 text.

Target support: current compiler targets.
