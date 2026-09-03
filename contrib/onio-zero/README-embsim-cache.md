# Running CoreMark under embsim, and testing the cache model

## Getting the simulator

Use a published binary rather than a local build, so a number can be traced to
a version:

```bash
EMBSIM=$(contrib/onio-zero/fetch-embsim.sh)
"$EMBSIM" --version
./contrib/onio-zero/run-coremark-embsim.py --embsim "$EMBSIM" ...
```

`fetch-embsim.sh` pins a version, verifies the published SHA-256 before
unpacking, and caches under `~/.cache/embsim`. Pass a version to override:
`fetch-embsim.sh v0.2.0`.

The runner also needs `gdb-multiarch` (or a RISC-V-capable GDB passed with
`--gdb`). It starts the published simulator's GDB stub on loopback and sends
the measurement commands through `monitor`; it does not use the interactive
terminal debugger opened by `--debug`.

embsim is in a **private** repository, so its releases need credentials. The
browser download URL returns 404 without them, which reads as a missing file
rather than a missing token — the script uses the API asset endpoint instead
and needs `gh auth login`, or `GH_TOKEN` set, with read access to
`mikro-design/embsim`.

**Anything before v0.2.0 predates the cache work.** The 0.1.0 binaries were
built before the model existed in the form described here, and before the
way-prediction bug was found.

## What changed in v0.2.0

**The prediction is one state for the whole cache, not one per set.** The next
fetch probes the same physical way the last one used, even when it indexes a
different set. The old per-set behaviour predicted correctly far more often
than any real predictor does, and it changed *which way a full-set fill
replaces* — so cache contents diverged from that point on rather than the
counts merely being wrong.

**The model is named for what it does.** `aldebaran-icache` is now
`wp-icache`; the parameters are unchanged. `embsim-onio-zero-coremark.json`
has been updated. An old name now fails loudly:

```
Unknown memif "aldebaran-icache"; available models: flat, cache, wp-icache
```

**Counters are reported and resettable.** `reset_counter` zeroes the memory
model's counters while *keeping the cache warm*, so a timed window measures
the machine the firmware actually left behind rather than a cold one. Before,
it reset the processor's counters and left the cache's running, so start-up
traffic landed on whatever was being measured.

**The statistics format is an interface.** One counter per line, as
`Code memory <counter>: <value>` and `Data memory <counter>: <value>`. It is
pinned by a test in embsim precisely because a parser that stops matching
reports zero rather than an error. The runner requires the complete cache
counter set whenever the selected stub configures a cache, including
`word_fetches` and `split_word_fetches`; a missing counter fails the run.

Every CSV row records the embsim version, SHA-256 of the executable, and
SHA-256 of the model file, so results retain their simulator and configuration
provenance after they have been copied elsewhere.

## The cache is parameters, not a menu

There is one model. `cache` and `wp-icache` are presets over it, so these are
the same cache:

```
wp-icache:size=2048,line=32,ways=2,soft_cycles=1,hard_cycles=10
cache:size=2048,line=32,ways=2,lookup=predicted,replace=predicted,word=4,soft_cycles=1,hard_cycles=10
```

| Parameter | |
| --- | --- |
| `size`, `line`, `ways` | Geometry; sets and line size must be powers of two |
| `lookup` | `parallel`, or `predicted` for way prediction |
| `replace` | `lru`, or `predicted` to evict the most recently read way |
| `fill_policy` | Which empty way a fill takes when several are: `lowest` or `predicted` |
| `word` | Physical SRAM word width |
| `hit_cycles` / `soft_cycles` / `hard_cycles` | Predicted hit / other-way hit / line fill |
| `writeback_cycles` | Evicting a dirty line |
| `split_word_cycles` | Each extra SRAM word an access reads |
| `track_sets` | Report hard misses per set |

So an alternative hypothesis about the hardware is a configuration change, not
a patch. The model is selected in the stub file the runner passes with
`--model` (`embsim-onio-zero-coremark.json`), under `memif`:

```json
"memif": {
  "code": "wp-icache:size=2048,line=32,ways=2,soft_cycles=1,hard_cycles=10",
  "data": "flat:penalty=0"
}
```

To test whether the part evicts by age rather than by prediction, copy that
file, change one field, and run both:

```json
"code": "cache:size=2048,line=32,ways=2,lookup=predicted,replace=lru,word=4,soft_cycles=1,hard_cycles=10"
```

```bash
./contrib/onio-zero/run-coremark-embsim.py --embsim "$EMBSIM" --model my-variant.json ...
```

## The SRAM-word effect

The array is read a word at a time. A 32-bit instruction at `pc % 4 == 2`
spans two words — invisible at cache-line granularity, because both words are
usually in the same line and the tag lookup is the same, but a second read of
the array.

This is now counted, per your measurement of thousands per CoreMark iteration
with a difference between builds:

```
Code memory word_fetches: ...
Code memory split_word_fetches: ...
```

`split_word_cycles` charges for it and **defaults to zero**. That is
deliberate. What a split costs is not known, and a made-up number would appear
in a report indistinguishable from a measurement. The counters are the part a
compiler can act on; the cost is the part a board measurement has to supply.

To explore a hypothesis, set it in a copy of the model file and compare — but
label the result as conditional on the assumption:

```json
"code": "wp-icache:size=2048,line=32,ways=2,soft_cycles=1,hard_cycles=10,split_word_cycles=1"
```

## What is still uncalibrated

`hard_cycles=10` is a round number putting a fill an order of magnitude above
a hit. It is not measured from any part, and neither is `soft_cycles` or
`split_word_cycles`.

So a cycle total from this configuration **ranks two builds**. It does not
predict a board. A change the model scores at a fraction of a percent can cost
ten times that in hardware, because the model counts events and the board pays
for them at a price the model was never told. Closing that gap needs the exact
flashed ELFs and hardware measurements — it cannot be done from the simulator
side.

## Assumptions worth challenging

- **`fill_policy`.** With more than one way empty, embsim takes the lowest
  numbered. Whether the part does, or fills the predicted way, is not derivable
  from a warm cache — but the two leave the predictor in different places and
  diverge on the next access to a different set. Both settings are implemented
  and tested; a measurement can decide it.
- **`replace=predicted`.** That a full set evicts the most recently read way
  is from the description, not from a measurement.
