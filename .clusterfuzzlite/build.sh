#!/bin/bash -eu
#
# ClusterFuzzLite build script for the Stratavm container format library.
#
# Environment supplied by ClusterFuzzLite:
#   $CC / $CXX   - compiler (clang / clang++ with sanitizer wrapping)
#   $CFLAGS / $CXXFLAGS - sanitizer + coverage flags
#   $LIB_FUZZING_ENGINE  - e.g. -fsanitize=fuzzer or /path/to/libFuzzer.a
#   $OUT         - directory to place finished fuzzer binaries
#   $SRC         - root of the cloned repository
#
# This script must:
#   - compile every source file using only $SRC-relative paths
#   - write every fuzzer binary into $OUT
#   - not download anything from the network
#   - not prompt for input

set -eu

PROJ="$SRC"

INCLUDES="-I${PROJ}/include"

# Collect all library source files.
LIB_SRCS=(
    "${PROJ}/src/status.cpp"
    "${PROJ}/src/atom_pool.cpp"
    "${PROJ}/src/schema.cpp"
    "${PROJ}/src/value.cpp"
    "${PROJ}/src/section.cpp"
    "${PROJ}/src/heap.cpp"
    "${PROJ}/src/journal.cpp"
    "${PROJ}/src/replay_vm.cpp"
    "${PROJ}/src/xref_index.cpp"
    "${PROJ}/src/validator.cpp"
    "${PROJ}/src/loader.cpp"
    "${PROJ}/src/builder.cpp"
    "${PROJ}/src/env.cpp"
    "${PROJ}/src/samples.cpp"
    "${PROJ}/src/query_parser.cpp"
    "${PROJ}/src/query_eval.cpp"
    "${PROJ}/src/wire.cpp"
    "${PROJ}/src/compactor.cpp"
    "${PROJ}/src/diff.cpp"
    "${PROJ}/src/field_index.cpp"
    "${PROJ}/src/schema_registry.cpp"
)

# Compile each library source to an object file.
OBJ_DIR="${OUT}/objs"
mkdir -p "${OBJ_DIR}"

OBJS=()
for src in "${LIB_SRCS[@]}"; do
    base="$(basename "${src}" .cpp)"
    obj="${OBJ_DIR}/${base}.o"
    $CXX $CXXFLAGS -std=c++17 $INCLUDES -c "${src}" -o "${obj}"
    OBJS+=("${obj}")
done

# Build container_fuzzer.
$CXX $CXXFLAGS -std=c++17 $INCLUDES \
    "${PROJ}/fuzz/container_fuzzer.cc" \
    "${OBJS[@]}" \
    $LIB_FUZZING_ENGINE \
    -o "${OUT}/container_fuzzer"

# Build journal_fuzzer.
$CXX $CXXFLAGS -std=c++17 $INCLUDES \
    "${PROJ}/fuzz/journal_fuzzer.cc" \
    "${OBJS[@]}" \
    $LIB_FUZZING_ENGINE \
    -o "${OUT}/journal_fuzzer"

# Package the per-harness seed corpora as required by the OSS-Fuzz convention.
# ClusterFuzzLite picks up <fuzzer>_seed_corpus.zip from $OUT.
if [ -d "${PROJ}/fuzz/corpus/container_fuzzer" ]; then
    zip -j "${OUT}/container_fuzzer_seed_corpus.zip" \
        "${PROJ}/fuzz/corpus/container_fuzzer/"*
fi

if [ -d "${PROJ}/fuzz/corpus/journal_fuzzer" ]; then
    zip -j "${OUT}/journal_fuzzer_seed_corpus.zip" \
        "${PROJ}/fuzz/corpus/journal_fuzzer/"*
fi

# Build query_fuzzer.
$CXX $CXXFLAGS -std=c++17 $INCLUDES \
    "${PROJ}/fuzz/query_fuzzer.cc" \
    "${OBJS[@]}" \
    $LIB_FUZZING_ENGINE \
    -o "${OUT}/query_fuzzer"
