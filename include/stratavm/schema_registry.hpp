// schema_registry.hpp - versioned schema registry with migration planning.
//
// A SchemaRegistry holds multiple snapshots of a Schema (each with its own
// AtomPool) indexed by a monotonically-increasing version_id. When data encoded
// under an older version must be read by a consumer that knows only a newer
// version (or vice versa), compute_migration() derives the set of per-field
// transformations needed and apply_migration() executes them against a node
// table.
//
// Design intent: the registry itself is the source of truth for version
// metadata. It does NOT deal with file I/O directly; callers are expected to
// embed a serialized registry in the META section of a container or alongside
// the container.
#ifndef STRATAVM_SCHEMA_REGISTRY_HPP
#define STRATAVM_SCHEMA_REGISTRY_HPP

#include <cstdint>
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
// SchemaVersion - one registered version entry.
// ---------------------------------------------------------------------------

struct SchemaVersion {
  uint32_t    version_id   = 0;
  std::string description;
  Schema      schema;
  AtomPool    atoms;
};

// ---------------------------------------------------------------------------
// FieldMigration - a single field-level transformation between two versions.
// ---------------------------------------------------------------------------

struct FieldMigration {
  enum class Kind : uint8_t {
    AddField    = 0,  // field added in the new version (fill with default)
    RemoveField = 1,  // field removed in the new version (erase it)
    RenameField = 2,  // field renamed: same field_idx, different name atom
    ChangeType  = 3,  // field type changed (value must be converted)
  };

  Kind      migration_kind = Kind::AddField;
  KindId    kind_id        = kInvalidKind;
  FieldId   field_idx      = 0;
  AtomId    old_name_atom  = kInvalidAtom;  // for RenameField / ChangeType
  AtomId    new_name_atom  = kInvalidAtom;  // for RenameField
  ValueType old_type       = ValueType::Invalid;  // for ChangeType
  ValueType new_type       = ValueType::Invalid;  // for ChangeType
  Value     default_value;                         // for AddField
};

// ---------------------------------------------------------------------------
// MigrationPlan - the ordered list of transformations from one version to
// another.
// ---------------------------------------------------------------------------

struct MigrationPlan {
  uint32_t                     from_version = 0;
  uint32_t                     to_version   = 0;
  std::vector<FieldMigration>  migrations;

  bool is_forward() const { return to_version > from_version; }
  bool empty()      const { return migrations.empty(); }
};

// ---------------------------------------------------------------------------
// SchemaRegistry
// ---------------------------------------------------------------------------

class SchemaRegistry {
 public:
  SchemaRegistry() = default;

  // Register a schema version. Returns an error if version_id is already
  // registered (use a new id instead; in-place mutation is not supported).
  Status register_version(uint32_t version_id, const std::string& description,
                           Schema schema, AtomPool atoms);

  // Find a registered version by id. Returns nullptr if not found.
  const SchemaVersion* find(uint32_t version_id) const;

  // List all registered version IDs in ascending order.
  std::vector<uint32_t> version_ids() const;

  // Compute a migration plan from `from_version` to `to_version`.
  //
  // The algorithm compares the two schemas by matching kind names and field
  // names:
  //   - A field present in `from` but absent in `to` → RemoveField.
  //   - A field present in `to` but absent in `from` → AddField (default=null).
  //   - A field present in both with a different ValueType → ChangeType.
  //   - A field with the same name and type in both → no migration needed.
  //
  // Kind matching is done by name atom string comparison across the two atom
  // pools, since atom IDs are not stable across versions.
  Result<MigrationPlan> compute_migration(uint32_t from_version,
                                           uint32_t to_version) const;

  // Apply a migration plan to a copy of `nodes`, returning the mutated copy.
  // The original node table is not modified.
  //
  // `from_schema` and `to_schema` must match the versions used when the plan
  // was computed. The caller is responsible for ensuring consistency.
  Result<std::vector<Node>> apply_migration(
      const std::vector<Node>& nodes,
      const MigrationPlan&     plan,
      const Schema&            from_schema,
      const Schema&            to_schema) const;

  // Serialize the registry to a self-contained byte blob.
  //
  // Wire format (all integers as unsigned LEB128 varint unless noted):
  //   varint  version_count
  //   for each version (ascending order):
  //     u32        version_id  (fixed 4 bytes, little-endian)
  //     lp_string  description
  //     varint     atom_count
  //     for each atom:
  //       lp_string  atom_text
  //     varint     kind_count
  //     for each kind:
  //       varint  kind_name_atom_idx   (index into THIS version's atom table)
  //       varint  field_count
  //       for each field:
  //         varint  field_name_atom_idx
  //         u8      value_type
  //         u8      flags
  std::vector<uint8_t> serialize() const;

  // Deserialize a registry from bytes produced by serialize().
  static Result<SchemaRegistry> deserialize(ByteReader& r, const Limits& lim);

  size_t version_count() const { return versions_.size(); }

 private:
  // Versions are stored sorted by version_id (std::map guarantees this).
  std::map<uint32_t, SchemaVersion> versions_;

  // Helper used by compute_migration: build a map from kind-name-string to
  // KindId for the given (schema, atoms) pair.
  static std::map<std::string, KindId> build_kind_name_map(
      const Schema& schema, const AtomPool& atoms);

  // Helper: build a map from field-name-string to FieldId within a KindDef.
  static std::map<std::string, FieldId> build_field_name_map(
      const KindDef& kind, const AtomPool& atoms);
};

}  // namespace stratavm

#endif  // STRATAVM_SCHEMA_REGISTRY_HPP
