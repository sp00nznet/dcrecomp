# Contributors

## Issue reports and testing

### @d3r2000

[#1](https://github.com/sp00nznet/dcrecomp/issues/1): pointed out that
`sh4_disasm.py`'s function map came only from `bsr` targets and prologues, so
targets reached through `jsr/jmp @Rn`, `braf`/`bsrf` tables and function-pointer
tables could be missing, and suggested comparing runtime dispatch misses against
the static map. The recompiler already handled most of those, but the map did
not, and the per-title fallback list was hardcoded into the toolkit. Fixed in
#2: one shared discovery pass and a per-title `--entries` file fed from the
dispatcher's unresolved-call log.
