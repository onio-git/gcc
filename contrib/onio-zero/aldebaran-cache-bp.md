# Aldebaran instruction-cache and branch-predictor notes

Source: `cache_bp (1).odt`, supplied by ONiO and dated 2025-04-06 in the ODT
metadata.

SHA-256: `b59836be5d00378379973b8da467d1f56f476c9bc1cfda4948e1464afdf90d0e`

This is a normalized engineering transcription.  The source document calls
the preferred way “last recently used” or “LRU”.  Its described behavior is
actually *most recently read*: that way is probed first and is also replaced
on a full hard miss.  The distinction matters to cache simulation.

## Instruction cache

- Capacity: 2 KiB.
- Organization: two ways, 32 lines per way, eight 32-bit words per line.
- Line size: 32 bytes.
- Physical storage: one 256 by 32-bit SRAM per way.
- Address bits 9:2 select the line and word.  Equivalently, bits 4:2 select a
  word and bits 9:5 select one of 32 sets.
- Address bits 19:10 form the tag.  Bits 1:0 are unused because SRAM accesses
  are 32 bits wide.
- Code addresses separated by 1 KiB have the same set index.

For each fetch, the cache probes the way in which the preceding valid
instruction was found, or into which the preceding flash fill was placed.

- Preferred-way hit: one cycle and one SRAM read.
- Soft miss: the line is in the other way; add one cycle and one SRAM read.
  Flash is not accessed.
- Hard miss: neither way contains the line.  The cache pays the soft-miss cost
  plus a full-line flash fetch cost.
- A hard miss uses an invalid way if one is available.  If both ways are
  valid, it replaces the preferred (most recently read) way.

The source does not quantify flash-line fill latency or energy.  Values used
for those quantities in a simulator are calibration parameters, not hardware
specification.

## Branch predictor

- Eight entries.
- Each entry holds a conditional-branch instruction address, a destination
  address, and a valid bit.
- Fetch looks up the current instruction address.  A matching valid entry
  redirects fetch to its destination on the next cycle.  A correct prediction
  saves one cycle of branch penalty.
- Decode verifies the instruction address, destination, and actual taken
  outcome.  A not-taken result or destination mismatch invalidates the entry.
- A taken branch with no matching entry is inserted.  An invalid slot is used
  first; if all entries are valid, the oldest entry is replaced.
- An entry is not inserted when another valid entry matches all but the bottom
  two instruction-address bits.  Consequently, of two compressed conditional
  branches in one aligned 32-bit instruction word, only one can be resident.
- Forward conditional branches are never stored in the predictor.

## Compiler consequences

- Make the likely edge of a forward conditional branch fall through whenever
  profile evidence supports it; a taken forward branch can never become a
  predictor hit.
- Keep the dynamic hot set of backward conditional branches near or below
  eight.  Unrolling can reduce branch executions but may evict code from the
  2 KiB cache, so its benefit must be measured on complete binaries.
- Avoid placing two important compressed conditional branches in the same
  aligned four-byte word.  Padding every branch is not justified: it consumes
  cache capacity and may create 1 KiB conflicts.  Apply this only with profile
  evidence.
- Code density is unusually valuable.  Alignment must be justified against
  32-byte line crossings and 1 KiB set aliases, not selected from generic
  desktop defaults.
- Function/block placement can matter even when total code size is unchanged:
  hot lines separated by 1 KiB compete for the same two ways, and the full-set
  replacement policy replaces the most recently read way.

## Still required from hardware validation

- Hard-miss line-fill latency and its dependence on flash state or clock.
- Energy per flash fill, SRAM access, predicted branch, and misprediction.
- Whether every ISA extension currently implied by `-mcpu=onio-zero` is
  implemented on all production Aldebaran revisions.  This cache/predictor
  document does not establish ISA support.
