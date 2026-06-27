# Makefile for local development and test builds.
# Requires clang++ with C++17 support.
#
# Targets:
#   make            - build the library and CLI tool
#   make tests      - build and run all unit tests
#   make gen-corpus - write seed corpus files to fuzz/corpus/
#   make clean      - remove build outputs

CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined
INCLUDES  = -Iinclude

BUILD_DIR = build

LIB_SRCS = \
    src/status.cpp \
    src/atom_pool.cpp \
    src/schema.cpp \
    src/value.cpp \
    src/section.cpp \
    src/heap.cpp \
    src/journal.cpp \
    src/replay_vm.cpp \
    src/xref_index.cpp \
    src/validator.cpp \
    src/loader.cpp \
    src/builder.cpp \
    src/env.cpp \
    src/samples.cpp \
    src/query_parser.cpp \
    src/query_eval.cpp \
    src/wire.cpp \
    src/compactor.cpp \
    src/diff.cpp \
    src/field_index.cpp \
    src/schema_registry.cpp

LIB_OBJS = $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(LIB_SRCS))

TEST_SRCS = \
    tests/test_byte_rw.cpp \
    tests/test_atom_schema.cpp \
    tests/test_loader.cpp \
    tests/test_replay.cpp \
    tests/test_builder.cpp \
    tests/test_query.cpp \
    tests/test_wire.cpp \
    tests/test_compactor.cpp \
    tests/test_diff.cpp \
    tests/test_field_index.cpp \
    tests/test_schema_registry.cpp

TEST_BINS = $(patsubst tests/%.cpp,$(BUILD_DIR)/%,$(TEST_SRCS))

all: $(BUILD_DIR)/stratavm

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/stratavm: tools/stratavm_cli.cpp $(LIB_OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Test binaries link the test source against all library objects.
$(BUILD_DIR)/test_%: tests/test_%.cpp $(LIB_OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Itests $^ -o $@

tests: $(TEST_BINS)
	@echo "=== Running unit tests ==="
	@failed=0; \
	for t in $(TEST_BINS); do \
	    echo "--- $$t ---"; \
	    $$t || failed=$$((failed+1)); \
	done; \
	if [ $$failed -gt 0 ]; then echo "$$failed test(s) FAILED"; exit 1; fi; \
	echo "All tests passed."

$(BUILD_DIR)/gen_corpus: tools/gen_corpus.cpp $(LIB_OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

gen-corpus: $(BUILD_DIR)/gen_corpus
	mkdir -p fuzz/corpus/container_fuzzer fuzz/corpus/journal_fuzzer
	$(BUILD_DIR)/gen_corpus fuzz/corpus/container_fuzzer fuzz/corpus/journal_fuzzer
	@echo "Seed corpus written."

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all tests gen-corpus clean
