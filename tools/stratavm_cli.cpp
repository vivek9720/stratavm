// stratavm_cli.cpp - a small command-line front end for the library.
//
// Subcommands:
//   info <file>        header + section directory + top-level counts
//   dump <file>        reconstruct the graph and print every node
//   validate <file>    strict validation; non-zero exit if the graph is invalid
//   build <name> <out> write a built-in sample container to disk
//   samples            list the available sample names
//
// The tool only reads/writes files named on the command line; it never touches
// the network and takes no interactive input.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "stratavm/loader.hpp"
#include "stratavm/samples.hpp"

namespace {

using namespace stratavm;

bool read_file(const std::string& path, std::vector<std::uint8_t>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  std::streamoff len = in.tellg();
  if (len < 0) return false;
  in.seekg(0, std::ios::beg);
  out->resize(static_cast<std::size_t>(len));
  if (len > 0) in.read(reinterpret_cast<char*>(out->data()), len);
  return static_cast<bool>(in) || in.eof();
}

bool write_file(const std::string& path, const std::vector<std::uint8_t>& data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  if (!data.empty())
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
  return static_cast<bool>(out);
}

const char* tag_name(SectionTag t) {
  switch (t) {
    case SectionTag::Atom: return "ATOM";
    case SectionTag::Schema: return "SCMA";
    case SectionTag::Heap: return "HEAP";
    case SectionTag::Journal: return "JRNL";
    case SectionTag::Xref: return "XREF";
    case SectionTag::Meta: return "META";
    default: return "????";
  }
}

std::string atom_text(const LoadedContainer& lc, AtomId id) {
  const std::string* s = lc.atoms.get(id);
  return s ? *s : (id == kInvalidAtom ? std::string("<anon>") : std::string("<oob>"));
}

int cmd_info(const std::string& path) {
  std::vector<std::uint8_t> bytes;
  if (!read_file(path, &bytes)) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    return 2;
  }
  LoadOptions opt;
  auto res = load(bytes.data(), bytes.size(), opt);
  if (res.is_error()) {
    std::printf("load failed: %s\n", res.status().to_string().c_str());
    return 1;
  }
  const LoadedContainer& lc = *res.value();
  std::printf("magic OK, version %u.%u, flags 0x%04x\n",
              lc.sections.header().version_major,
              lc.sections.header().version_minor, lc.sections.header().flags);
  std::printf("sections: %zu\n", lc.sections.entries().size());
  for (const auto& e : lc.sections.entries()) {
    std::printf("  %-4s  off=%-8u len=%-8u v=%u\n", tag_name(e.kind()), e.offset,
                e.length, e.version);
  }
  std::printf("atoms=%zu kinds=%zu nodes=%zu\n", lc.atoms.size(),
              lc.schema.kind_count(), lc.vm.node_count());
  const ReplayStats& st = lc.vm.stats();
  std::printf("ops=%u created=%u placeholders=%u dropped=%u links=%u snapshots=%u\n",
              st.ops_executed, st.nodes_created, st.placeholders, st.nodes_dropped,
              st.children_linked, st.snapshots);
  std::printf("xref_warnings=%u\n", lc.xref_warnings);
  return 0;
}

int cmd_dump(const std::string& path) {
  std::vector<std::uint8_t> bytes;
  if (!read_file(path, &bytes)) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    return 2;
  }
  LoadOptions opt;
  auto res = load(bytes.data(), bytes.size(), opt);
  if (res.is_error()) {
    std::printf("load failed: %s\n", res.status().to_string().c_str());
    return 1;
  }
  const LoadedContainer& lc = *res.value();
  for (const Node& n : lc.vm.nodes()) {
    const KindDef* k = lc.schema.kind(n.kind);
    std::string kind_name = k ? atom_text(lc, k->name) : std::string("<placeholder>");
    std::printf("#%u %s name=%s%s%s\n", n.id, kind_name.c_str(),
                atom_text(lc, n.name).c_str(), n.alive ? "" : " [dropped]",
                n.placeholder ? " [placeholder]" : "");
    for (const auto& kv : n.fields) {
      std::printf("    field[%u] = %s\n", kv.first,
                  kv.second.to_string(lc.atoms).c_str());
    }
    if (!n.children.empty()) {
      std::printf("    children:");
      for (NodeId c : n.children) std::printf(" #%u", c);
      std::printf("\n");
    }
  }
  const ValidationReport& rep = lc.report;
  std::printf("validation: live=%u dangling_refs=%u unfilled=%u missing_req=%u issues=%zu\n",
              rep.live_nodes, rep.dangling_refs, rep.unfilled_placeholders,
              rep.missing_required, rep.issues.size());
  return 0;
}

int cmd_validate(const std::string& path) {
  std::vector<std::uint8_t> bytes;
  if (!read_file(path, &bytes)) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    return 2;
  }
  LoadOptions opt;
  opt.strict_validation = true;
  auto res = load(bytes.data(), bytes.size(), opt);
  if (res.is_error()) {
    std::printf("load failed: %s\n", res.status().to_string().c_str());
    return 1;
  }
  const ValidationReport& rep = res.value()->report;
  for (const auto& issue : rep.issues) {
    std::printf("  issue node #%u %s: %s\n", issue.node, code_name(issue.code),
                issue.detail.c_str());
  }
  std::printf("%zu issue(s)\n", rep.issues.size());
  return rep.ok() ? 0 : 1;
}

int cmd_build(const std::string& name, const std::string& out) {
  std::vector<std::uint8_t> bytes;
  if (name == "minimal") bytes = sample_container_minimal();
  else if (name == "groups") bytes = sample_container_groups();
  else if (name == "rich") bytes = sample_container_rich();
  else if (name == "journal_basic") bytes = sample_journal_basic();
  else if (name == "journal_groups") bytes = sample_journal_groups();
  else {
    std::fprintf(stderr, "unknown sample '%s'\n", name.c_str());
    return 2;
  }
  if (!write_file(out, bytes)) {
    std::fprintf(stderr, "cannot write %s\n", out.c_str());
    return 2;
  }
  std::printf("wrote %zu bytes to %s\n", bytes.size(), out.c_str());
  return 0;
}

void usage() {
  std::printf(
      "usage: stratavm <command> [args]\n"
      "  info <file>\n"
      "  dump <file>\n"
      "  validate <file>\n"
      "  build <minimal|groups|rich|journal_basic|journal_groups> <out>\n"
      "  samples\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  std::string cmd = argv[1];
  if (cmd == "info" && argc == 3) return cmd_info(argv[2]);
  if (cmd == "dump" && argc == 3) return cmd_dump(argv[2]);
  if (cmd == "validate" && argc == 3) return cmd_validate(argv[2]);
  if (cmd == "build" && argc == 4) return cmd_build(argv[2], argv[3]);
  if (cmd == "samples") {
    std::printf("minimal groups rich journal_basic journal_groups\n");
    return 0;
  }
  usage();
  return 2;
}
