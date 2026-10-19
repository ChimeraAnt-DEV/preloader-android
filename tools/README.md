# Automated signature-rule generation

The preloader reaches every game seam by RTTI, because the game strips its
symbols and its byte patterns drift between builds. Deriving a rule by hand is
what the earlier rounds of work did, one build at a time. `tools/gen_rules.py`
does the mechanical part in one step.

## What it derives

For each seam the preloader resolves by RTTI name, the tool finds the slot's
address so a rule can pin the slot index and prove the seam exists:

| Type name | Slot | Rule key |
| --- | --- | --- |
| `18LivePlayerRenderer` | 17 | `playerRenderVtableIndex` |
| `20ClientNetworkHandler` | 40 | `packetReadVtableIndex` |
| `14ClientInstance` | 31 | (probe only; not a rule field) |

## Why a plain byte scan finds nothing

RTTI pointers live in `.data.rel.ro` as `R_AARCH64_RELATIVE` addends, not as
literal pointers. On disk that section holds zeros; the loader writes the
addends at load time. A byte scan for the typeinfo pointer therefore finds
nothing and produces a false "RTTI is absent" conclusion. The tool rebuilds
`.rela.dyn` first (writes each `R_AARCH64_RELATIVE` addend at its target), so a
static read matches what the process sees. That is the entire trick.

The type-name string is standalone in `.rodata`; the typeinfo *struct* begins
one word before the word that points at it, and the vtable's address point is
one word past the typeinfo pointer in the struct.

## Usage

```
tools/gen_rules.py --apk <file-or-url> --min 1.26.60.28 [--max 1.26.60.28] \
    --out rule.json
```

Verified against `minecraft-26-60-28.apk`: `18LivePlayerRenderer` slot 17 ->
`0xaeaefb4`, `20ClientNetworkHandler` slot 40 -> `0xb80f518`, matching the
values recovered earlier by hand.

## Fail-closed by construction

A seam whose slot does not resolve to executable `.text` is reported and
**omitted** from the rule. It must stay absent so the runtime fails closed
instead of detouring a data pointer. The tool never guesses an address.

## Probe-backed rules (item 17)

Each emitted rule carries a `probes` block naming, per seam, whether it
resolved, the address, and what a success proves. A rule that turns a feature
on must have a resolved probe for the seam that feature needs; a rule with no
resolved probe for a seam leaves that feature off. The generator enforces this
by only writing the rule key when the probe resolved.
