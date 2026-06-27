# Stratavm

Stratavm is a binary container format library for structured object graphs with
a stateful replay journal. It provides:

- A deterministic binary container format (`.svm`) built around a section
  directory (similar in concept to PE/ELF), an interned atom pool, a schema
  section, an initial heap snapshot, a journal of replay operations, a cross-
  reference index, and a metadata block.
- A journal replay VM that processes a sequence of typed opcodes (`NEW_NODE`,
  `SET_FIELD`, `DROP_NODE`, `BEGIN_GROUP`/`ADD_CHILD`/`END_GROUP`, `LINK_REF`,
  `SNAPSHOT`, `SET_META`, `NOP`) against a mutable node graph.
- A group-construction stack with forward-reference materialisation: when a
  `BEGIN_GROUP` instruction names children that do not yet exist as nodes, the
  VM defers them and materialises placeholder nodes at `END_GROUP` time.
- A cross-reference reconciler and graph validator.
- A `ContainerBuilder` for round-trip construction and serialisation.
- Two libFuzzer harnesses and realistic seed corpora.

## Repository layout

```
include/stratavm/   Public headers (format spec, all core types)
src/                Library implementation (~96 KiB of C++17 source)
fuzz/               libFuzzer harnesses + seed corpora + dictionary
  corpus/container_fuzzer/   5 seed containers (minimal/groups/rich/fwdref/xref)
  corpus/journal_fuzzer/     5 seed journal byte-streams
  container_fuzzer.cc        Drives stratavm::load() end-to-end
  journal_fuzzer.cc          Drives ReplayVM with a seeded root node
  dictionary.txt             Format tokens for guided mutation
tests/              Unit tests (43 cases, zero external dependencies)
tools/              CLI, corpus generator, PoC generator, standalone replay driver
.clusterfuzzlite/   ClusterFuzzLite build script and project.yaml
Makefile            Local build (clang++-16, ASan+UBSan)
```

## Building locally

Requires: `clang++-16` (or any C++17-capable clang with `libclang-rt` ASan), `make`, `zip`.

```sh
make           # build library + tests with ASan+UBSan
make tests     # run the 43-case test suite
make gen-corpus  # regenerate seed corpus under fuzz/corpus/
```

All build steps are fully offline and deterministic. No network access or
external dependencies are required.

## Fuzz targets

| Target | Exercises |
|---|---|
| `container_fuzzer` | Full end-to-end `load()`: magic check, section directory, atom pool, schema, heap decoder, replay VM, xref reconciler, validator |
| `journal_fuzzer` | Journal replay VM in isolation with a seeded root node; focuses mutation budget on opcode decoding, group stack, forward-reference materialisation |

## ClusterFuzzLite integration

See `.clusterfuzzlite/build.sh` and `.clusterfuzzlite/project.yaml`.
The build script compiles all library sources using the `$CXX`/`$CXXFLAGS`
provided by ClusterFuzzLite, links each harness against `$LIB_FUZZING_ENGINE`,
and zips the seed corpora into `$OUT/<target>_seed_corpus.zip`.

## Format overview

A `.svm` file has the structure:

```
[FileHeader: 16 bytes]         magic "STRATAVM" | version u8 | reserved | section_count u16
[SectionEntry × N: 16 bytes each]  tag u8 | flags u8 | reserved | offset u32 | length u32 | checksum u32
[Section payloads...]
```

Supported section tags: `ATOM` (atom pool), `SCMA` (schema), `HEAP` (initial
node snapshot), `JRNL` (journal), `XREF` (cross-reference index), `META`
(key/value metadata).

All multi-byte integers are little-endian. Lengths/indices use unsigned LEB128
varints. Signed values use zig-zag LEB128. Floating-point values are IEEE-754
doubles stored as little-endian u64 bit patterns.

## Known bug

There is one intentional, unpatched memory-safety bug in this codebase. It
lives in the group-construction path of the replay VM. A PoC that triggers it
is maintained outside the repository. Do not patch it until a Fenrir submission
has been filed.
