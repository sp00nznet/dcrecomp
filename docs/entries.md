# Entry points and the `--entries` file

`static_recompile.py` only compiles code it knows is code. It finds function
starts from:

- `bsr` targets and `sts.l pr,@-r15` prologues;
- `mov.l @(disp,PC),Rn` followed by `jsr/jmp @Rn` (literal-pool calls);
- `mova` + `mov.w @(r0,Rn)` + `braf` switch tables (`_jump_table_targets`);
- any 32-bit word in the binary that points at an even address inside it
  (vtables, callback tables) - kept as *mid-function entries*, not new
  function boundaries, so loops that span them still compile
  (`_resolve_mid_entries`).

`sh4_disasm.py` uses the same discovery, so its `*_functions.json` lists what
the recompiler will compile.

What none of that can see is an address the game computes at runtime. Those go
in a per-title text file, kept in the game repo, and passed with `--entries`:

```
# entries.txt - one address per line, '#' starts a comment
0x8C148990  # Main loop entry (BRA target from init)
```

```
python dcrecomp/tools/static_recompile.py disc_extract/1ST_READ.BIN src/game include/game --entries entries.txt
```

They used to be a hardcoded list in `static_recompile.py`, which forced Crazy
Taxi's addresses into every other title's build.

## Finding the missing ones

A call through the dispatcher to an address with no compiled function logs:

```
[DISPATCH] unresolved call to 0x8C15B282 (pr=0x8C15B1F0)
```

Add each address to `entries.txt` and recompile:

```
grep -o "unresolved call to 0x[0-9A-Fa-f]*" run.log | awk '{print $4}' | sort -u >> entries.txt
```

The log stops after 100 lines, so a long list takes more than one run.

## Measured on Crazy Taxi (US)

| | functions | mid-function entries |
|---|---|---|
| old `sh4_disasm.py` map (bsr + prologue only) | 11,561 | - |
| `static_recompile.py`, no `--entries` | 12,727 | 13,459 |
| `static_recompile.py --entries entries.txt` (50 addresses) | 12,750 | 13,442 |

The last row is byte-identical to the output from before the list moved out of
the toolkit.
