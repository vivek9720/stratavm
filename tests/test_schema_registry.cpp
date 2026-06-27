// test_schema_registry.cpp - unit tests for SchemaRegistry.
#include "test_runner.hpp"

#include "stratavm/atom_pool.hpp"
#include "stratavm/byte_reader.hpp"
#include "stratavm/byte_writer.hpp"
#include "stratavm/format.hpp"
#include "stratavm/node.hpp"
#include "stratavm/schema.hpp"
#include "stratavm/schema_registry.hpp"
#include "stratavm/status.hpp"
#include "stratavm/value.hpp"

#include <algorithm>
#include <vector>

using namespace stratavm;
using namespace stratavm::test;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a Schema with a single kind having a given set of (name, type) fields.
// `atoms` is populated in-place.
struct FieldSpec {
  const char* name;
  ValueType   type;
};

static Schema build_schema(AtomPool& atoms,
                             const char* kind_name,
                             const std::vector<FieldSpec>& fields) {
  AtomId a_kind = atoms.intern(kind_name);

  ByteWriter w;
  w.varint(1);        // 1 kind
  w.varint(a_kind);   // kind name

  w.varint(static_cast<uint64_t>(fields.size()));
  for (const FieldSpec& fs : fields) {
    AtomId a_field = atoms.intern(fs.name);
    w.varint(a_field);
    w.u8(static_cast<uint8_t>(fs.type));
    w.u8(0);  // flags
  }

  auto bytes = w.take();
  ByteReader r(bytes.data(), bytes.size());
  Schema schema;
  Limits lim;
  (void)schema.decode(r, atoms, lim);  // errors are fatal in test helpers
  return schema;
}

// ---------------------------------------------------------------------------
// Test: register_version and find()
// ---------------------------------------------------------------------------

STEST(registry_register_and_find) {
  SchemaRegistry reg;

  AtomPool atoms1;
  Schema   s1 = build_schema(atoms1, "node", {{"id", ValueType::Int}});
  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(atoms1)));

  const SchemaVersion* sv = reg.find(1);
  REQUIRE(sv != nullptr);
  REQUIRE_EQ(sv->version_id, 1u);
  REQUIRE_EQ(sv->description, std::string("v1"));

  // Non-existent version returns nullptr.
  REQUIRE(reg.find(99) == nullptr);
}

// ---------------------------------------------------------------------------
// Test: version_ids() returns sorted IDs
// ---------------------------------------------------------------------------

STEST(registry_version_ids) {
  SchemaRegistry reg;

  AtomPool a1, a3, a2;
  Schema   s1 = build_schema(a1, "kind1", {{"f", ValueType::Int}});
  Schema   s3 = build_schema(a3, "kind1", {{"f", ValueType::Int}});
  Schema   s2 = build_schema(a2, "kind1", {{"f", ValueType::Int}});

  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(a1)));
  REQUIRE_OK(reg.register_version(3, "v3", std::move(s3), std::move(a3)));
  REQUIRE_OK(reg.register_version(2, "v2", std::move(s2), std::move(a2)));

  auto ids = reg.version_ids();
  REQUIRE_EQ(ids.size(), 3u);
  REQUIRE_EQ(ids[0], 1u);
  REQUIRE_EQ(ids[1], 2u);
  REQUIRE_EQ(ids[2], 3u);
}

// ---------------------------------------------------------------------------
// Test: registering the same version twice returns an error
// ---------------------------------------------------------------------------

STEST(registry_duplicate_version) {
  SchemaRegistry reg;

  AtomPool a1, a2;
  Schema   s1 = build_schema(a1, "kind1", {{"f", ValueType::Int}});
  Schema   s2 = build_schema(a2, "kind1", {{"f", ValueType::Int}});

  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(a1)));

  Status st = reg.register_version(1, "v1-dup", std::move(s2), std::move(a2));
  REQUIRE(st.is_error());
  REQUIRE_EQ(st.code(), Code::InvariantViolation);
}

// ---------------------------------------------------------------------------
// Test: compute_migration — same schema produces empty plan
// ---------------------------------------------------------------------------

STEST(registry_compute_migration_same) {
  SchemaRegistry reg;

  AtomPool a1, a2;
  Schema   s1 = build_schema(a1, "node", {{"id", ValueType::Int},
                                            {"tag", ValueType::Atom}});
  Schema   s2 = build_schema(a2, "node", {{"id", ValueType::Int},
                                            {"tag", ValueType::Atom}});

  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(a1)));
  REQUIRE_OK(reg.register_version(2, "v2", std::move(s2), std::move(a2)));

  auto res = reg.compute_migration(1, 2);
  REQUIRE(res.is_ok());
  const MigrationPlan& plan = res.value();
  REQUIRE_EQ(plan.from_version, 1u);
  REQUIRE_EQ(plan.to_version,   2u);
  REQUIRE(plan.empty());
}

// ---------------------------------------------------------------------------
// Test: compute_migration detects AddField (new field in v2)
// ---------------------------------------------------------------------------

STEST(registry_compute_migration_add_field) {
  SchemaRegistry reg;

  AtomPool a1, a2;
  // v1: kind "node" with 1 field "id"
  Schema s1 = build_schema(a1, "node", {{"id", ValueType::Int}});
  // v2: kind "node" with 2 fields "id" + "label"
  Schema s2 = build_schema(a2, "node", {{"id",    ValueType::Int},
                                          {"label", ValueType::Atom}});

  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(a1)));
  REQUIRE_OK(reg.register_version(2, "v2", std::move(s2), std::move(a2)));

  auto res = reg.compute_migration(1, 2);
  REQUIRE(res.is_ok());
  const MigrationPlan& plan = res.value();

  // Should have exactly one migration: AddField for "label".
  REQUIRE_EQ(plan.migrations.size(), 1u);
  REQUIRE_EQ(plan.migrations[0].migration_kind, FieldMigration::Kind::AddField);
  REQUIRE(plan.is_forward());
}

// ---------------------------------------------------------------------------
// Test: compute_migration detects RemoveField (field removed in v2)
// ---------------------------------------------------------------------------

STEST(registry_compute_migration_remove_field) {
  SchemaRegistry reg;

  AtomPool a1, a2;
  // v1: kind "node" with 2 fields
  Schema s1 = build_schema(a1, "node", {{"id",  ValueType::Int},
                                          {"tag", ValueType::Atom}});
  // v2: kind "node" with 1 field ("tag" removed)
  Schema s2 = build_schema(a2, "node", {{"id", ValueType::Int}});

  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(a1)));
  REQUIRE_OK(reg.register_version(2, "v2", std::move(s2), std::move(a2)));

  auto res = reg.compute_migration(1, 2);
  REQUIRE(res.is_ok());
  const MigrationPlan& plan = res.value();

  // Should have exactly one migration: RemoveField for "tag".
  REQUIRE_EQ(plan.migrations.size(), 1u);
  REQUIRE_EQ(plan.migrations[0].migration_kind, FieldMigration::Kind::RemoveField);
}

// ---------------------------------------------------------------------------
// Test: compute_migration detects ChangeType
// ---------------------------------------------------------------------------

STEST(registry_compute_migration_change_type) {
  SchemaRegistry reg;

  AtomPool a1, a2;
  // v1: "score" is Int
  Schema s1 = build_schema(a1, "node", {{"score", ValueType::Int}});
  // v2: "score" is Float
  Schema s2 = build_schema(a2, "node", {{"score", ValueType::Float}});

  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(a1)));
  REQUIRE_OK(reg.register_version(2, "v2", std::move(s2), std::move(a2)));

  auto res = reg.compute_migration(1, 2);
  REQUIRE(res.is_ok());
  const MigrationPlan& plan = res.value();

  REQUIRE_EQ(plan.migrations.size(), 1u);
  REQUIRE_EQ(plan.migrations[0].migration_kind, FieldMigration::Kind::ChangeType);
  REQUIRE_EQ(plan.migrations[0].old_type, ValueType::Int);
  REQUIRE_EQ(plan.migrations[0].new_type, ValueType::Float);
}

// ---------------------------------------------------------------------------
// Test: compute_migration — missing version returns error
// ---------------------------------------------------------------------------

STEST(registry_compute_migration_missing_version) {
  SchemaRegistry reg;

  AtomPool a1;
  Schema s1 = build_schema(a1, "node", {{"id", ValueType::Int}});
  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(a1)));

  // to_version 99 does not exist.
  auto res = reg.compute_migration(1, 99);
  REQUIRE(res.is_error());
  REQUIRE_EQ(res.status().code(), Code::NotFound);

  // from_version 0 does not exist.
  auto res2 = reg.compute_migration(0, 1);
  REQUIRE(res2.is_error());
  REQUIRE_EQ(res2.status().code(), Code::NotFound);
}

// ---------------------------------------------------------------------------
// Test: apply_migration — AddField sets default on existing nodes
// ---------------------------------------------------------------------------

STEST(registry_apply_migration_add_field) {
  SchemaRegistry reg;

  AtomPool a1, a2;
  Schema s1 = build_schema(a1, "node", {{"id", ValueType::Int}});
  Schema s2 = build_schema(a2, "node", {{"id",    ValueType::Int},
                                          {"label", ValueType::Atom}});

  Schema s1_copy = build_schema(a1, "node", {{"id", ValueType::Int}});
  Schema s2_copy = build_schema(a2, "node", {{"id",    ValueType::Int},
                                               {"label", ValueType::Atom}});

  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(a1)));
  REQUIRE_OK(reg.register_version(2, "v2", std::move(s2), std::move(a2)));

  // Build a migration plan.
  auto plan_res = reg.compute_migration(1, 2);
  REQUIRE(plan_res.is_ok());
  const MigrationPlan& plan = plan_res.value();
  REQUIRE_EQ(plan.migrations.size(), 1u);
  REQUIRE_EQ(plan.migrations[0].migration_kind, FieldMigration::Kind::AddField);

  // Build some nodes under v1 schema: kind 0 (the only kind), field 0 = "id".
  std::vector<Node> nodes;
  Node n0;
  n0.id = 0; n0.kind = 0; n0.name = kInvalidAtom;
  n0.alive = true; n0.placeholder = false; n0.generation = 0;
  n0.set_field(0, Value::make_int(100));
  nodes.push_back(n0);

  Node n1;
  n1.id = 1; n1.kind = 0; n1.name = kInvalidAtom;
  n1.alive = true; n1.placeholder = false; n1.generation = 0;
  n1.set_field(0, Value::make_int(200));
  nodes.push_back(n1);

  auto migrated_res = reg.apply_migration(nodes, plan, s1_copy, s2_copy);
  REQUIRE(migrated_res.is_ok());

  const std::vector<Node>& migrated = migrated_res.value();
  REQUIRE_EQ(migrated.size(), 2u);

  // field 0 should be unchanged.
  REQUIRE(migrated[0].field(0) != nullptr);
  REQUIRE_EQ(migrated[0].field(0)->as_int(), int64_t{100});

  // field 1 should have been added with the default (Invalid) value.
  REQUIRE(migrated[0].field(1) != nullptr);
  REQUIRE(!migrated[0].field(1)->valid());  // default = Invalid

  REQUIRE(migrated[1].field(1) != nullptr);
  REQUIRE(!migrated[1].field(1)->valid());
}

// ---------------------------------------------------------------------------
// Test: apply_migration — RemoveField erases the field from nodes
// ---------------------------------------------------------------------------

STEST(registry_apply_migration_remove_field) {
  SchemaRegistry reg;

  AtomPool a1, a2;
  Schema s1 = build_schema(a1, "node", {{"id",  ValueType::Int},
                                          {"tag", ValueType::Atom}});
  Schema s2 = build_schema(a2, "node", {{"id", ValueType::Int}});

  Schema s1_ref = build_schema(a1, "node", {{"id",  ValueType::Int},
                                              {"tag", ValueType::Atom}});
  Schema s2_ref = build_schema(a2, "node", {{"id", ValueType::Int}});

  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(a1)));
  REQUIRE_OK(reg.register_version(2, "v2", std::move(s2), std::move(a2)));

  auto plan_res = reg.compute_migration(1, 2);
  REQUIRE(plan_res.is_ok());

  AtomPool tag_atoms;
  AtomId a_tag = tag_atoms.intern("hello");

  std::vector<Node> nodes;
  Node n0;
  n0.id = 0; n0.kind = 0; n0.alive = true; n0.placeholder = false; n0.generation = 0;
  n0.set_field(0, Value::make_int(42));
  n0.set_field(1, Value::make_atom(a_tag));
  nodes.push_back(n0);

  auto migrated_res =
      reg.apply_migration(nodes, plan_res.value(), s1_ref, s2_ref);
  REQUIRE(migrated_res.is_ok());

  const std::vector<Node>& migrated = migrated_res.value();
  REQUIRE_EQ(migrated.size(), 1u);

  // field 0 ("id") should survive.
  REQUIRE(migrated[0].field(0) != nullptr);

  // field 1 ("tag") should have been erased.
  REQUIRE(migrated[0].field(1) == nullptr);
}

// ---------------------------------------------------------------------------
// Test: apply_migration — ChangeType Int→Float converts value
// ---------------------------------------------------------------------------

STEST(registry_apply_migration_change_type_int_to_float) {
  SchemaRegistry reg;

  AtomPool a1, a2;
  Schema s1 = build_schema(a1, "node", {{"score", ValueType::Int}});
  Schema s2 = build_schema(a2, "node", {{"score", ValueType::Float}});
  Schema s1_ref = build_schema(a1, "node", {{"score", ValueType::Int}});
  Schema s2_ref = build_schema(a2, "node", {{"score", ValueType::Float}});

  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(a1)));
  REQUIRE_OK(reg.register_version(2, "v2", std::move(s2), std::move(a2)));

  auto plan_res = reg.compute_migration(1, 2);
  REQUIRE(plan_res.is_ok());

  std::vector<Node> nodes;
  Node n0;
  n0.id = 0; n0.kind = 0; n0.alive = true; n0.placeholder = false; n0.generation = 0;
  n0.set_field(0, Value::make_int(7));
  nodes.push_back(n0);

  auto migrated_res = reg.apply_migration(nodes, plan_res.value(), s1_ref, s2_ref);
  REQUIRE(migrated_res.is_ok());

  const std::vector<Node>& migrated = migrated_res.value();
  REQUIRE(migrated[0].field(0) != nullptr);
  REQUIRE_EQ(migrated[0].field(0)->type(), ValueType::Float);
  // 7 as int → 7.0 as float.
  REQUIRE(migrated[0].field(0)->as_float() == 7.0);
}

// ---------------------------------------------------------------------------
// Test: serialize / deserialize round-trip
// ---------------------------------------------------------------------------

STEST(registry_serialize_deserialize) {
  SchemaRegistry reg;

  AtomPool a1, a2;
  Schema s1 = build_schema(a1, "widget", {{"x", ValueType::Int},
                                            {"y", ValueType::Int}});
  Schema s2 = build_schema(a2, "widget", {{"x",     ValueType::Int},
                                            {"y",     ValueType::Int},
                                            {"label", ValueType::Atom}});

  REQUIRE_OK(reg.register_version(10, "version ten",   std::move(s1), std::move(a1)));
  REQUIRE_OK(reg.register_version(20, "version twenty", std::move(s2), std::move(a2)));

  // Serialize to bytes.
  auto bytes = reg.serialize();
  REQUIRE(!bytes.empty());

  // Deserialize back.
  ByteReader r(bytes.data(), bytes.size());
  Limits lim;
  auto res = SchemaRegistry::deserialize(r, lim);
  REQUIRE(res.is_ok());

  const SchemaRegistry& reg2 = res.value();
  REQUIRE_EQ(reg2.version_count(), 2u);

  // Both versions present.
  const SchemaVersion* sv10 = reg2.find(10);
  const SchemaVersion* sv20 = reg2.find(20);
  REQUIRE(sv10 != nullptr);
  REQUIRE(sv20 != nullptr);

  REQUIRE_EQ(sv10->description, std::string("version ten"));
  REQUIRE_EQ(sv20->description, std::string("version twenty"));

  // Check kind count survived.
  REQUIRE_EQ(sv10->schema.kind_count(), 1u);
  REQUIRE_EQ(sv20->schema.kind_count(), 1u);

  // Check field counts survived.
  const KindDef* k10 = sv10->schema.kind(0);
  const KindDef* k20 = sv20->schema.kind(0);
  REQUIRE(k10 != nullptr);
  REQUIRE(k20 != nullptr);
  REQUIRE_EQ(k10->fields.size(), 2u);
  REQUIRE_EQ(k20->fields.size(), 3u);
}

// ---------------------------------------------------------------------------
// Test: version_count() reflects registered versions
// ---------------------------------------------------------------------------

STEST(registry_version_count) {
  SchemaRegistry reg;
  REQUIRE_EQ(reg.version_count(), 0u);

  AtomPool a1;
  Schema s1 = build_schema(a1, "n", {{"f", ValueType::Int}});
  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(a1)));
  REQUIRE_EQ(reg.version_count(), 1u);

  AtomPool a2;
  Schema s2 = build_schema(a2, "n", {{"f", ValueType::Int}});
  REQUIRE_OK(reg.register_version(2, "v2", std::move(s2), std::move(a2)));
  REQUIRE_EQ(reg.version_count(), 2u);
}

// ---------------------------------------------------------------------------
// Test: apply_migration — dead nodes are not modified
// ---------------------------------------------------------------------------

STEST(registry_apply_migration_skip_dead) {
  SchemaRegistry reg;

  AtomPool a1, a2;
  Schema s1 = build_schema(a1, "node", {{"id", ValueType::Int}});
  Schema s2 = build_schema(a2, "node", {{"id",    ValueType::Int},
                                          {"extra", ValueType::Atom}});
  Schema s1_ref = build_schema(a1, "node", {{"id", ValueType::Int}});
  Schema s2_ref = build_schema(a2, "node", {{"id",    ValueType::Int},
                                              {"extra", ValueType::Atom}});

  REQUIRE_OK(reg.register_version(1, "v1", std::move(s1), std::move(a1)));
  REQUIRE_OK(reg.register_version(2, "v2", std::move(s2), std::move(a2)));

  auto plan_res = reg.compute_migration(1, 2);
  REQUIRE(plan_res.is_ok());

  std::vector<Node> nodes;

  // Dead node — should not get the new field.
  Node n0;
  n0.id = 0; n0.kind = 0; n0.alive = false; n0.placeholder = false; n0.generation = 0;
  n0.set_field(0, Value::make_int(1));
  nodes.push_back(n0);

  // Alive node — should get the new field.
  Node n1;
  n1.id = 1; n1.kind = 0; n1.alive = true; n1.placeholder = false; n1.generation = 0;
  n1.set_field(0, Value::make_int(2));
  nodes.push_back(n1);

  auto migrated_res = reg.apply_migration(nodes, plan_res.value(), s1_ref, s2_ref);
  REQUIRE(migrated_res.is_ok());

  const std::vector<Node>& migrated = migrated_res.value();
  REQUIRE_EQ(migrated.size(), 2u);

  // Dead node: field 1 must NOT have been added.
  REQUIRE(migrated[0].field(1) == nullptr);

  // Alive node: field 1 was added with default value.
  REQUIRE(migrated[1].field(1) != nullptr);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) { return stratavm::test::run_all(argc, argv); }
