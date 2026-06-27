#include "stratavm/validator.hpp"

namespace stratavm {

namespace {

void add_issue(ValidationReport* rep, Code code, NodeId node, std::string detail) {
  rep->issues.push_back(ValidationIssue{code, node, std::move(detail)});
}

}  // namespace

ValidationReport validate_graph(const ReplayVM& vm, const Schema& schema,
                                const AtomPool& atoms, bool strict) {
  ValidationReport rep;
  const std::vector<Node>& nodes = vm.nodes();
  const NodeId count = static_cast<NodeId>(nodes.size());

  for (const Node& n : nodes) {
    if (n.placeholder) {
      // A placeholder that was never resolved to a real definition is a dangling
      // forward reference.
      rep.unfilled_placeholders++;
      if (strict)
        add_issue(&rep, Code::UnresolvedRef, n.id, "unfilled placeholder");
      continue;
    }
    if (!n.alive) continue;
    rep.live_nodes++;

    const KindDef* kind = schema.kind(n.kind);
    if (kind == nullptr) {
      add_issue(&rep, Code::KindOutOfRange, n.id, "live node with bad kind");
      continue;
    }

    // Required fields must be present.
    for (FieldId fi = 0; fi < kind->fields.size(); ++fi) {
      const FieldDef& fd = kind->fields[fi];
      if (fd.is_required() && n.field(fi) == nullptr) {
        rep.missing_required++;
        add_issue(&rep, Code::InvariantViolation, n.id,
                  "missing required field " + atoms.text_or(fd.name, "?"));
      }
    }

    // Outgoing references must point at live, in-range nodes.
    for (NodeId t : n.refs) {
      if (t >= count) {
        rep.dangling_refs++;
        if (strict) add_issue(&rep, Code::UnresolvedRef, n.id, "ref out of range");
      } else if (!nodes[t].alive) {
        rep.dangling_refs++;
        if (strict) add_issue(&rep, Code::UnresolvedRef, n.id, "ref to dropped node");
      }
    }

    // Children must be in range.
    for (NodeId c : n.children) {
      if (c >= count) {
        rep.dangling_refs++;
        if (strict) add_issue(&rep, Code::UnresolvedRef, n.id, "child out of range");
      }
    }
  }
  return rep;
}

}  // namespace stratavm
