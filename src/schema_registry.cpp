// schema_registry.cpp - implementation of SchemaRegistry.
#include "stratavm/schema_registry.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <string>
#include <vector>

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/byte_writer.hpp"
#include "stratavm/format.hpp"
#include "stratavm/node.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/status.hpp"
#include "stratavm/value.hpp"

namespace stratavm {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

std::map<std::string, KindId> SchemaRegistry::build_kind_name_map(
    const Schema& schema, const AtomPool& atoms) {
  std::map<std::string, KindId> result;
  for (KindId kid = 0; kid < static_cast<KindId>(schema.kind_count()); ++kid) {
    const KindDef* k = schema.kind(kid);
    if (!k) continue;
    const std::string* name = atoms.get(k->name);
    if (!name) continue;
    result[*name] = kid;
  }
  return result;
}

std::map<std::string, FieldId> SchemaRegistry::build_field_name_map(
    const KindDef& kind, const AtomPool& atoms) {
  std::map<std::string, FieldId> result;
  for (FieldId fid = 0; fid < static_cast<FieldId>(kind.fields.size()); ++fid) {
    const FieldDef& fd = kind.fields[fid];
    const std::string* name = atoms.get(fd.name);
    if (!name) continue;
    result[*name] = fid;
  }
  return result;
}

// ---------------------------------------------------------------------------
// register_version
// ---------------------------------------------------------------------------

Status SchemaRegistry::register_version(uint32_t version_id,
                                         const std::string& description,
                                         Schema schema, AtomPool atoms) {
  if (versions_.count(version_id)) {
    return fail(Code::InvariantViolation,
                "schema version already registered: " +
                    std::to_string(version_id));
  }
  SchemaVersion sv;
  sv.version_id   = version_id;
  sv.description  = description;
  sv.schema       = std::move(schema);
  sv.atoms        = std::move(atoms);
  versions_.emplace(version_id, std::move(sv));
  return Status::ok();
}

// ---------------------------------------------------------------------------
// find
// ---------------------------------------------------------------------------

const SchemaVersion* SchemaRegistry::find(uint32_t version_id) const {
  auto it = versions_.find(version_id);
  return it == versions_.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// version_ids
// ---------------------------------------------------------------------------

std::vector<uint32_t> SchemaRegistry::version_ids() const {
  // std::map iterates in ascending key order, so we get sorted IDs for free.
  std::vector<uint32_t> ids;
  ids.reserve(versions_.size());
  for (const auto& [id, _] : versions_) {
    ids.push_back(id);
  }
  return ids;
}

// ---------------------------------------------------------------------------
// compute_migration
// ---------------------------------------------------------------------------

Result<MigrationPlan> SchemaRegistry::compute_migration(
    uint32_t from_version, uint32_t to_version) const {
  const SchemaVersion* from_sv = find(from_version);
  if (!from_sv)
    return fail(Code::NotFound,
                "from_version not found: " + std::to_string(from_version));

  const SchemaVersion* to_sv = find(to_version);
  if (!to_sv)
    return fail(Code::NotFound,
                "to_version not found: " + std::to_string(to_version));

  MigrationPlan plan;
  plan.from_version = from_version;
  plan.to_version   = to_version;

  const Schema& from_schema = from_sv->schema;
  const Schema& to_schema   = to_sv->schema;
  const AtomPool& from_atoms = from_sv->atoms;
  const AtomPool& to_atoms   = to_sv->atoms;

  // Build name→KindId maps for both sides.
  auto from_kinds = build_kind_name_map(from_schema, from_atoms);
  auto to_kinds   = build_kind_name_map(to_schema,   to_atoms);

  // --- Process every kind that exists in the `from` schema ---
  for (const auto& [kind_name, from_kid] : from_kinds) {
    const KindDef* from_kdef = from_schema.kind(from_kid);
    if (!from_kdef) continue;

    auto to_it = to_kinds.find(kind_name);
    if (to_it == to_kinds.end()) {
      // Kind was entirely removed in `to`: every field is RemoveField.
      for (FieldId fid = 0;
           fid < static_cast<FieldId>(from_kdef->fields.size()); ++fid) {
        const FieldDef& fd = from_kdef->fields[fid];
        FieldMigration m;
        m.migration_kind = FieldMigration::Kind::RemoveField;
        m.kind_id        = from_kid;
        m.field_idx      = fid;
        m.old_name_atom  = fd.name;
        m.old_type       = fd.type;
        plan.migrations.push_back(std::move(m));
      }
      continue;
    }

    // Kind exists in both versions — compare fields by name.
    KindId to_kid = to_it->second;
    const KindDef* to_kdef = to_schema.kind(to_kid);
    if (!to_kdef) continue;

    auto from_fields = build_field_name_map(*from_kdef, from_atoms);
    auto to_fields   = build_field_name_map(*to_kdef,   to_atoms);

    // Fields in `from` but not in `to` → RemoveField.
    for (const auto& [field_name, from_fid] : from_fields) {
      if (to_fields.count(field_name) == 0) {
        const FieldDef& fd = from_kdef->fields[from_fid];
        FieldMigration m;
        m.migration_kind = FieldMigration::Kind::RemoveField;
        m.kind_id        = from_kid;
        m.field_idx      = from_fid;
        m.old_name_atom  = fd.name;
        m.old_type       = fd.type;
        plan.migrations.push_back(std::move(m));
      }
    }

    // Fields in `to` but not in `from` → AddField (default = null/Invalid).
    for (const auto& [field_name, to_fid] : to_fields) {
      if (from_fields.count(field_name) == 0) {
        const FieldDef& fd = to_kdef->fields[to_fid];
        FieldMigration m;
        m.migration_kind = FieldMigration::Kind::AddField;
        m.kind_id        = from_kid;  // same logical kind
        m.field_idx      = to_fid;
        m.new_name_atom  = fd.name;
        m.new_type       = fd.type;
        // default_value remains default-constructed (Invalid).
        plan.migrations.push_back(std::move(m));
      }
    }

    // Fields present in both: check for type changes.
    for (const auto& [field_name, from_fid] : from_fields) {
      auto tf_it = to_fields.find(field_name);
      if (tf_it == to_fields.end()) continue;  // handled above

      FieldId to_fid = tf_it->second;
      const FieldDef& from_fd = from_kdef->fields[from_fid];
      const FieldDef& to_fd   = to_kdef->fields[to_fid];

      if (from_fd.type != to_fd.type) {
        FieldMigration m;
        m.migration_kind = FieldMigration::Kind::ChangeType;
        m.kind_id        = from_kid;
        m.field_idx      = from_fid;
        m.old_name_atom  = from_fd.name;
        m.new_name_atom  = to_fd.name;
        m.old_type       = from_fd.type;
        m.new_type       = to_fd.type;
        plan.migrations.push_back(std::move(m));
      }
    }
  }

  // --- Process kinds only in `to` (brand new kinds) ---
  for (const auto& [kind_name, to_kid] : to_kinds) {
    if (from_kinds.count(kind_name)) continue;  // already handled above

    const KindDef* to_kdef = to_schema.kind(to_kid);
    if (!to_kdef) continue;

    // Every field in the new kind is an AddField relative to the old version.
    // We use kInvalidKind as kind_id since there is no corresponding from-kind.
    for (FieldId fid = 0;
         fid < static_cast<FieldId>(to_kdef->fields.size()); ++fid) {
      const FieldDef& fd = to_kdef->fields[fid];
      FieldMigration m;
      m.migration_kind = FieldMigration::Kind::AddField;
      m.kind_id        = kInvalidKind;  // no from-kind mapping
      m.field_idx      = fid;
      m.new_name_atom  = fd.name;
      m.new_type       = fd.type;
      plan.migrations.push_back(std::move(m));
    }
  }

  return plan;
}

// ---------------------------------------------------------------------------
// apply_migration
// ---------------------------------------------------------------------------

Result<std::vector<Node>> SchemaRegistry::apply_migration(
    const std::vector<Node>& nodes,
    const MigrationPlan&     plan,
    const Schema&            /*from_schema*/,
    const Schema&            /*to_schema*/) const {
  // Work on a copy so the caller's data is not mutated.
  std::vector<Node> result = nodes;

  for (Node& node : result) {
    if (!node.alive) continue;

    for (const FieldMigration& m : plan.migrations) {
      // Only apply migrations that match this node's kind.
      // Migrations with kind_id == kInvalidKind target newly-added kinds and
      // do not apply to existing nodes.
      if (m.kind_id != node.kind) continue;

      switch (m.migration_kind) {
        case FieldMigration::Kind::RemoveField:
          node.fields.erase(m.field_idx);
          break;

        case FieldMigration::Kind::AddField:
          // Only add the default if the field is not already present.
          if (node.fields.find(m.field_idx) == node.fields.end()) {
            node.set_field(m.field_idx, m.default_value);
          }
          break;

        case FieldMigration::Kind::RenameField:
          // The field index stays the same; the name lives in the schema, not
          // the node, so there is nothing to change in the node's field map.
          // (This migration kind is recorded for schema documentation purposes.)
          break;

        case FieldMigration::Kind::ChangeType: {
          auto it = node.fields.find(m.field_idx);
          if (it == node.fields.end()) break;

          const Value& old_val = it->second;
          Value new_val;

          // Attempt best-effort type conversion.
          if (m.old_type == ValueType::Int &&
              m.new_type == ValueType::Atom) {
            // Int → Atom: represent the integer as its decimal string.
            // We cannot intern into the pool here (no mutable pool), so we
            // produce an Invalid value to signal that manual fixup is needed.
            // The caller can post-process if they have a mutable AtomPool.
            (void)old_val;
            new_val = Value();  // Invalid — needs manual fixup
          } else if (m.old_type == ValueType::Atom &&
                     m.new_type == ValueType::Int) {
            // Atom → Int: not possible without atom pool access here;
            // produce 0 as a safe default.
            new_val = Value::make_int(0);
          } else if (m.old_type == ValueType::Int &&
                     m.new_type == ValueType::Float) {
            new_val = Value::make_float(static_cast<double>(old_val.as_int()));
          } else if (m.old_type == ValueType::Float &&
                     m.new_type == ValueType::Int) {
            new_val = Value::make_int(static_cast<int64_t>(old_val.as_float()));
          } else {
            // Unknown conversion: leave as Invalid (field effectively removed).
            new_val = Value();
          }

          node.set_field(m.field_idx, std::move(new_val));
          break;
        }
      }
    }
  }

  return result;
}

// ---------------------------------------------------------------------------
// serialize
// ---------------------------------------------------------------------------

std::vector<uint8_t> SchemaRegistry::serialize() const {
  ByteWriter w;

  // Write version count.
  w.varint(static_cast<uint64_t>(versions_.size()));

  for (const auto& [vid, sv] : versions_) {
    // version_id as fixed u32 (little-endian).
    w.u32(sv.version_id);

    // Human-readable description.
    w.lp_string(sv.description);

    // Atom pool: count followed by strings.
    w.varint(static_cast<uint64_t>(sv.atoms.size()));
    for (AtomId aid = 0; aid < static_cast<AtomId>(sv.atoms.size()); ++aid) {
      const std::string* s = sv.atoms.get(aid);
      if (s) {
        w.lp_string(*s);
      } else {
        w.lp_string("");  // shouldn't happen, but be safe
      }
    }

    // Schema: kind count followed by kind definitions.
    w.varint(static_cast<uint64_t>(sv.schema.kind_count()));
    for (KindId kid = 0;
         kid < static_cast<KindId>(sv.schema.kind_count()); ++kid) {
      const KindDef* kdef = sv.schema.kind(kid);
      if (!kdef) {
        // Degenerate — write empty kind.
        w.varint(static_cast<uint64_t>(kInvalidAtom));
        w.varint(0u);
        continue;
      }
      w.varint(static_cast<uint64_t>(kdef->name));
      w.varint(static_cast<uint64_t>(kdef->fields.size()));
      for (const FieldDef& fd : kdef->fields) {
        w.varint(static_cast<uint64_t>(fd.name));
        w.u8(static_cast<uint8_t>(fd.type));
        w.u8(fd.flags);
      }
    }
  }

  return w.take();
}

// ---------------------------------------------------------------------------
// deserialize
// ---------------------------------------------------------------------------

Result<SchemaRegistry> SchemaRegistry::deserialize(ByteReader& r,
                                                    const Limits& lim) {
  SchemaRegistry reg;

  uint64_t version_count = 0;
  {
    Status s = r.read_varint(&version_count);
    if (s.is_error()) return s;
  }

  if (version_count > lim.max_sections) {
    return fail(Code::LimitExceeded, "schema registry: version count too large");
  }

  for (uint64_t vi = 0; vi < version_count; ++vi) {
    SchemaVersion sv;

    // Fixed u32 version_id.
    {
      Status s = r.read_u32(&sv.version_id);
      if (s.is_error()) return s;
    }

    // Description string.
    {
      Status s = r.read_lp_string(&sv.description, lim.max_atom_len);
      if (s.is_error()) return s;
    }

    // Atom pool.
    uint64_t atom_count = 0;
    {
      Status s = r.read_varint(&atom_count);
      if (s.is_error()) return s;
    }
    if (atom_count > lim.max_atoms)
      return fail(Code::LimitExceeded, "atom count too large in schema registry");

    for (uint64_t ai = 0; ai < atom_count; ++ai) {
      std::string text;
      {
        Status s = r.read_lp_string(&text, lim.max_atom_len);
        if (s.is_error()) return s;
      }
      sv.atoms.intern(text);
    }

    // Schema.
    uint64_t kind_count = 0;
    {
      Status s = r.read_varint(&kind_count);
      if (s.is_error()) return s;
    }
    if (kind_count > lim.max_kinds)
      return fail(Code::LimitExceeded, "kind count too large in schema registry");

    for (uint64_t ki = 0; ki < kind_count; ++ki) {
      KindDef kdef;
      uint32_t kind_name_raw = 0;
      {
        Status s = r.read_varint_u32(&kind_name_raw);
        if (s.is_error()) return s;
      }
      kdef.name = kind_name_raw;

      uint64_t field_count = 0;
      {
        Status s = r.read_varint(&field_count);
        if (s.is_error()) return s;
      }
      if (field_count > lim.max_fields_per_kind)
        return fail(Code::LimitExceeded, "field count too large in schema registry");

      kdef.fields.reserve(static_cast<size_t>(field_count));
      for (uint64_t fi = 0; fi < field_count; ++fi) {
        FieldDef fd;
        uint32_t fname_raw = 0;
        {
          Status s = r.read_varint_u32(&fname_raw);
          if (s.is_error()) return s;
        }
        fd.name = fname_raw;

        uint8_t type_raw = 0;
        {
          Status s = r.read_u8(&type_raw);
          if (s.is_error()) return s;
        }
        if (!is_valid_value_type(type_raw))
          return fail(Code::BadSchema, "unknown field type in schema registry");
        fd.type = static_cast<ValueType>(type_raw);

        {
          Status s = r.read_u8(&fd.flags);
          if (s.is_error()) return s;
        }
        kdef.fields.push_back(fd);
      }
      sv.schema.add_kind(kdef);
    }

    // Register without error-checking duplicate IDs since a well-formed
    // serialized registry should not have duplicates.
    Status s = reg.register_version(sv.version_id, sv.description,
                                    std::move(sv.schema), std::move(sv.atoms));
    if (s.is_error()) return s;
  }

  return reg;
}

}  // namespace stratavm
