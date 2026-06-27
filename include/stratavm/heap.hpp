// heap.hpp - the initial object heap (the HEAP section).
//
// HEAP provides the base state that exists before the journal runs. It is a flat
// list of fully-formed nodes: id, kind, optional name, and a set of typed field
// values. Decoding validates each field against the node's kind via the schema,
// so a node can never carry a field its kind does not declare and a value is
// always read with the correct width.
#ifndef STRATAVM_HEAP_HPP
#define STRATAVM_HEAP_HPP

#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/node.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/status.hpp"

namespace stratavm {

struct HeapImage {
  std::vector<Node> nodes;
};

// Decode the HEAP payload into `out`. Node ids are assigned positionally (the
// on-disk id is validated to equal its index, which keeps later references
// unambiguous). Field decoding is schema-driven.
Status decode_heap(ByteReader& r, const Schema& schema, const AtomPool& atoms,
                   const Limits& limits, HeapImage* out);

}  // namespace stratavm

#endif  // STRATAVM_HEAP_HPP
