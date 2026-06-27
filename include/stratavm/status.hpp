// status.hpp - structured error handling for the Stratavm loader/VM.
//
// The codebase deliberately avoids exceptions on the hot decode paths. Instead
// every fallible operation returns a Status (an error code plus a short human
// readable detail) or a Result<T> wrapper. This keeps the fuzz target free of
// unwinding noise and makes the failure points easy to reason about.
#ifndef STRATAVM_STATUS_HPP
#define STRATAVM_STATUS_HPP

#include <cstdint>
#include <string>
#include <utility>

namespace stratavm {

// Error categories used throughout the decoder and replay machine. The values
// are stable so that the CLI can print them and tests can assert on them.
enum class Code : std::uint16_t {
  Ok = 0,
  Truncated,         // ran off the end of the input buffer
  BadMagic,          // file header magic did not match
  UnsupportedVersion,
  BadSectionTable,   // section directory is malformed or overlaps
  DuplicateSection,
  MissingSection,
  BadAtomPool,
  BadSchema,
  BadHeap,
  BadJournal,
  BadXref,
  BadValue,
  BadOpcode,
  NodeOutOfRange,
  AtomOutOfRange,
  KindOutOfRange,
  FieldOutOfRange,
  TypeMismatch,
  GroupUnderflow,    // END_GROUP without a matching BEGIN_GROUP
  GroupOverflow,     // frame stack nesting limit exceeded
  UnresolvedRef,
  InvariantViolation,
  LimitExceeded,     // a configured decode limit was hit
  Internal,
  BadChecksum,       // CRC or integrity check failed
  NotFound,          // a named entity was not found in a registry/index
  AlreadyExists,     // attempted to register a duplicate entry
};

// Returns a stable, lowercase-ish token for a code. Used by the CLI and tests.
const char* code_name(Code c);

// A lightweight error value. `Ok` carries no detail string; everything else may
// optionally attach a short context message describing where it went wrong.
class Status {
 public:
  Status() : code_(Code::Ok) {}
  Status(Code c) : code_(c) {}
  Status(Code c, std::string detail) : code_(c), detail_(std::move(detail)) {}

  static Status ok() { return Status(); }

  bool is_ok() const { return code_ == Code::Ok; }
  bool is_error() const { return code_ != Code::Ok; }
  explicit operator bool() const { return is_ok(); }

  Code code() const { return code_; }
  const std::string& detail() const { return detail_; }

  // Render the status as "code_name: detail" (or just "ok").
  std::string to_string() const;

 private:
  Code code_;
  std::string detail_;
};

// Convenience constructors that keep call sites terse.
inline Status fail(Code c) { return Status(c); }
inline Status fail(Code c, std::string detail) { return Status(c, std::move(detail)); }

// Result<T> couples a value with a status. When the status is an error the value
// is left default-constructed and must not be read. The decoder threads these
// through by value; T is expected to be cheap to move.
template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)), status_(Status::ok()) {}
  Result(Status status) : value_(), status_(std::move(status)) {}

  bool is_ok() const { return status_.is_ok(); }
  bool is_error() const { return status_.is_error(); }
  explicit operator bool() const { return is_ok(); }

  const Status& status() const { return status_; }

  // Only valid when is_ok(). Callers are expected to check first.
  T& value() { return value_; }
  const T& value() const { return value_; }
  T&& take() { return std::move(value_); }

 private:
  T value_;
  Status status_;
};

// Propagate an error out of the current function if `expr` evaluates to an error
// status. Mirrors the common TRY/ASSIGN_OR_RETURN idiom but without macros that
// leak across translation units.
#define STRATAVM_RETURN_IF_ERROR(expr)            \
  do {                                            \
    ::stratavm::Status _stratavm_s = (expr);      \
    if (_stratavm_s.is_error()) return _stratavm_s; \
  } while (0)

}  // namespace stratavm

#endif  // STRATAVM_STATUS_HPP
