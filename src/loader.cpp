#include "stratavm/loader.hpp"

namespace stratavm {

namespace {

// Decode the optional META section: varint count followed by (key, value) atom
// pairs. Stored alongside the journal-applied metadata.
Status decode_meta_section(ByteReader& r, const AtomPool& atoms,
                           const Limits& limits,
                           std::unordered_map<AtomId, AtomId>* out) {
  std::uint32_t count = 0;
  STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&count));
  if (count > limits.max_atoms)
    return fail(Code::LimitExceeded, "meta count too large");
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t key = 0, value = 0;
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&key));
    STRATAVM_RETURN_IF_ERROR(r.read_varint_u32(&value));
    if (atoms.get(key) == nullptr)
      return fail(Code::AtomOutOfRange, "meta key");
    if (value != kInvalidAtom && atoms.get(value) == nullptr)
      return fail(Code::AtomOutOfRange, "meta value");
    (*out)[key] = value;
  }
  return Status::ok();
}

// Carve a sub-reader for a section entry.
Result<ByteReader> section_reader(const ByteReader& base, const SectionEntry& e) {
  return base.window(e.offset, e.length);
}

}  // namespace

Result<std::unique_ptr<LoadedContainer>> load(const std::uint8_t* data,
                                              std::size_t size,
                                              const LoadOptions& options) {
  auto lc = std::make_unique<LoadedContainer>(options.limits);
  const Limits& limits = lc->limits;

  ByteReader base(data, size);

  // 1. Section directory.
  STRATAVM_RETURN_IF_ERROR(lc->sections.decode(base, size, limits));

  // 2. Atom pool (optional but referenced by nearly everything else).
  if (const SectionEntry* e = lc->sections.find(SectionTag::Atom)) {
    Result<ByteReader> sr = section_reader(base, *e);
    if (sr.is_error()) return sr.status();
    ByteReader r = sr.take();
    STRATAVM_RETURN_IF_ERROR(lc->atoms.decode(r, limits));
  }

  // 3. Schema.
  if (const SectionEntry* e = lc->sections.find(SectionTag::Schema)) {
    Result<ByteReader> sr = section_reader(base, *e);
    if (sr.is_error()) return sr.status();
    ByteReader r = sr.take();
    STRATAVM_RETURN_IF_ERROR(lc->schema.decode(r, lc->atoms, limits));
  }

  // 4. Initial heap, used to seed the VM.
  if (const SectionEntry* e = lc->sections.find(SectionTag::Heap)) {
    Result<ByteReader> sr = section_reader(base, *e);
    if (sr.is_error()) return sr.status();
    ByteReader r = sr.take();
    HeapImage heap;
    STRATAVM_RETURN_IF_ERROR(decode_heap(r, lc->schema, lc->atoms, limits, &heap));
    lc->vm.seed(std::move(heap));
  }

  // 5. Replay the journal to reconstruct the final state.
  if (options.run_journal) {
    if (const SectionEntry* e = lc->sections.find(SectionTag::Journal)) {
      Result<ByteReader> sr = section_reader(base, *e);
      if (sr.is_error()) return sr.status();
      ByteReader r = sr.take();
      STRATAVM_RETURN_IF_ERROR(lc->vm.run(r));
      lc->journal_ran = true;
    }
  }

  // 6. Build the derived cross-reference index over the final graph.
  lc->xref.build(lc->vm.nodes());

  // 7. Reconcile the optional precomputed XREF section.
  if (const SectionEntry* e = lc->sections.find(SectionTag::Xref)) {
    Result<ByteReader> sr = section_reader(base, *e);
    if (sr.is_error()) return sr.status();
    ByteReader r = sr.take();
    STRATAVM_RETURN_IF_ERROR(lc->xref.decode_and_reconcile(
        r, lc->atoms, limits, lc->vm.node_count(), &lc->xref_warnings));
  }

  // 8. Optional standalone metadata section.
  if (const SectionEntry* e = lc->sections.find(SectionTag::Meta)) {
    Result<ByteReader> sr = section_reader(base, *e);
    if (sr.is_error()) return sr.status();
    ByteReader r = sr.take();
    STRATAVM_RETURN_IF_ERROR(
        decode_meta_section(r, lc->atoms, limits, &lc->section_meta));
  }

  // 9. Validate the reconstructed graph.
  lc->report = validate_graph(lc->vm, lc->schema, lc->atoms,
                              options.strict_validation);

  return Result<std::unique_ptr<LoadedContainer>>(std::move(lc));
}

}  // namespace stratavm
