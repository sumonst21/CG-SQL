/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Note: the terms "rowset" and "result set" are used pretty much interchangebly to mean the same thing.

#include <stdlib.h>

#if defined(TARGET_OS_LINUX) && TARGET_OS_LINUX
#include <alloca.h>
#endif // TARGET_OS_LINUX

#ifndef STACK_BYTES_ALLOC
#if defined(TARGET_OS_WIN32) && TARGET_OS_WIN32
#define STACK_BYTES_ALLOC(N, C) char *N = (char *)_alloca(C)
#elif defined(TARGET_OS_LINUX) && TARGET_OS_LINUX
#define STACK_BYTES_ALLOC(N, C) char *N = (char *)alloca(C)
#else // TARGET_OS_WIN32
#define STACK_BYTES_ALLOC(N, C) char N[C]
#endif // TARGET_OS_WIN32
#endif // STACK_BYTES_ALLLOC

// This code is used in the event of a THROW inside a stored proc.  When that happens
// we want to keep the result code we have if there was a recent error. If we recently
// got a success, then use SQLITE_ERROR as the thrown error instead.
cql_code cql_best_error(cql_code rc) {
  if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) {
    return SQLITE_ERROR;
  }
  return rc;
}

// This code overrides the CQL_DATA_TYPE_ENCODED bit in a result_set's data_type. It's used
// indirectly by app at runtime to control the encoding and decoding of the column value.
void cql_set_encoding(uint8_t *_Nonnull data_types, cql_int32 count, cql_int32 col, cql_bool encode) {
  cql_contract(col < count);
  if (encode) {
    data_types[col] |= CQL_DATA_TYPE_ENCODED;
  } else {
    data_types[col] &= ~CQL_DATA_TYPE_ENCODED;
  }
}

// The indicated statement should be immediately finalized out latest result was not SQLITE_OK
// This code is used during binding (which is now always done with multibind)
// in order to ensure that the statement exits finalized in the event of any binding failure.
void cql_finalize_on_error(cql_code rc, sqlite3_stmt *_Nullable *_Nonnull pstmt) {
  cql_contract(pstmt && *pstmt);
  if (rc != SQLITE_OK) {
    cql_finalize_stmt(pstmt);
  }
}

// This method is used when handling CQL cursors; the cursor local variable may already
// contain a statement.  When preparing a new statement, we want to finalize any statement
// the cursor used to hold.  This lets us do simple preparation in a loop without added
// conditionals in the generated code.
cql_code cql_prepare(sqlite3 *_Nonnull db, sqlite3_stmt *_Nullable *_Nonnull pstmt, const char *_Nonnull sql) {
  cql_finalize_stmt(pstmt);
  return cql_sqlite3_prepare_v2(db, sql, -1, pstmt, NULL);
}

// create a single string from the varargs and count provided
static char *_Nonnull cql_vconcat(cql_int32 count, const char *_Nullable preds, va_list *_Nonnull args) {
  va_list pass1, pass2;
  va_copy(pass1, *args);
  va_copy(pass2, *args);

  cql_int32 bytes = 0;

  // first we have to figure out how much to allocate
  for (cql_int32 istr = 0; istr < count; istr++) {
    const char *str = va_arg(pass1, const char *);
    if (!preds || preds[istr]) {
      bytes += strlen(str);
    }
  }

  char *result = malloc(bytes + 1);

  cql_int32 offset = 0;

  for (cql_int32 istr = 0; istr < count; istr++) {
    const char *str = va_arg(pass2, const char *);
    if (!preds || preds[istr]) {
      size_t len = strlen(str);
      memcpy(result + offset, str, len+1); // copies the trailing null byte
      offset += len;
    }
  }

  va_end(pass1);
  va_end(pass2);

  return result;
}

// This method is used when handling CQL cursors; the cursor local variable may already
// contain a statement.  When preparing a new statement, we want to finalize any statement
// the cursor used to hold.  This lets us do simple preparation in a loop without added
// conditionals in the generated code.  This is the varargs version
cql_code cql_prepare_var(
  sqlite3 *_Nonnull db,
  sqlite3_stmt *_Nullable *_Nonnull pstmt,
  cql_int32 count,
  const char *_Nullable preds, ...)
{
  cql_finalize_stmt(pstmt);
  va_list args;
  va_start(args, preds);
  char *sql = cql_vconcat(count, preds, &args);
  cql_code result = cql_sqlite3_prepare_v2(db, sql, -1, pstmt, NULL);
  va_end(args);
  free(sql);
  return result;
}

// This is a simple wrapper for the sqlite3_exec method with the usual extra arguments.
// This code is here just to reduce the code size of exec calls in the generated code.
// There are a lot of such calls.
cql_code cql_exec(sqlite3 *_Nonnull db, const char *_Nonnull sql) {
  return cql_sqlite3_exec(db, sql);
}

// This is a simple wrapper for the sqlite3_exec method with the usual extra arguments.
// This code is here just to reduce the code size of exec calls in the generated code.
// There are a lot of such calls.
cql_code cql_exec_var(sqlite3 *_Nonnull db, cql_int32 count, const char *_Nullable preds,...) {
  va_list args;
  va_start(args, preds);
  char *sql = cql_vconcat(count, preds, &args);
  cql_code result = cql_sqlite3_exec(db, sql);
  va_end(args);
  free(sql);
  return result;
}

// This version of exec takes a string variable and is therefore more dangerous.  It is
// only intended to be used in the context of schema maintenance or other cases where
// there are highly compressible patterns (like DROP TRIGGER %s for 1000s of triggers).
// All we do is convert the incoming string reference into a C string and then exec it.
CQL_WARN_UNUSED cql_code cql_exec_internal(sqlite3 *_Nonnull db, cql_string_ref _Nonnull str_ref) {
  cql_alloc_cstr(temp, str_ref);
  cql_code rc = cql_sqlite3_exec(db, temp);
  cql_free_cstr(temp, str_ref);
  return rc;
}

char *_Nonnull cql_address_of_col(
    cql_result_set_ref _Nonnull result_set,
    cql_int32 row,
    cql_int32 col,
    cql_int32 *_Nonnull type);

// The variable byte encoding is little endian, you stop when you reach
// a byte that does not have the high bit set.  This is good enough for 2^28 bits
// in four bytes which is more than enough for sql strings...
static const char *_Nonnull cql_decode(const char *_Nonnull data, int32_t *_Nonnull result) {
  int32_t out = 0;
  int32_t byte;
  int32_t offset = 0;
  do {
    byte = *data++;
    out |= (byte & 0x7f) << offset;
    offset += 7;
  } while (byte & 0x80);
  *result = out;
  return data;
}

// The base pointer contains the address of the string part
// Each fragment is variable length encoded as above with a +1 on the offset
// If an offset of 0 is encountered, that means stop.
// Since the fragements are represented as a string, that means the normal
// null terminator in the string is the stop signal.
static void cql_expand_frags(
  char *_Nonnull result,
  const char *_Nonnull base,
  const char *_Nonnull frags)
{
  int32_t offset;
  for (;;) {
    frags = cql_decode(frags, &offset);
    if (offset == 0) {
      break;
    }

    const char *src = base + offset - 1;
    while (*src) {
      *result++ = *src++;
    }
  }
  *result = 0;
}

// To keep the contract as simple as possible we encode everything we
// need into the fragment array.  Including the size of the output
// and fragment terminator.  See above.  This also makes the code
// gen as simple as possible.
cql_code cql_prepare_frags(
 sqlite3 *_Nonnull db,
 sqlite3_stmt *_Nullable *_Nonnull pstmt,
 const char *_Nonnull base,
 const char *_Nonnull frags)
{
  // NOTE: len is the allocation size (includes trailing \0)
  cql_finalize_stmt(pstmt);
  int32_t len;
  frags = cql_decode(frags, &len);
  STACK_BYTES_ALLOC(sql, len);
  cql_expand_frags(sql, base, frags);
  return cql_sqlite3_prepare_v2(db, sql, len, pstmt, NULL);
}

// To keep the contract as simple as possible we encode everything we
// need into the fragment array.  Including the size of the output
// and fragment terminator.  See above.  This also makes the code
// gen as simple as possible.
cql_code cql_exec_frags(
  sqlite3 *_Nonnull db,
  const char *_Nonnull base,
  const char *_Nonnull frags)
{
  // NOTE: len is the allocation size (includes trailing \0)
  int32_t len;
  frags = cql_decode(frags, &len);
  STACK_BYTES_ALLOC(sql, len);
  cql_expand_frags(sql, base, frags);
  return cql_sqlite3_exec(db, sql);
}

// Finalizes the statement if it is not null.  Note that the statement pointer
// must be not null but the statement it holds may or may not be initialized.
// Also note that ALL CQL STATEMENTS ARE INITIALIZED TO NULL!!
void cql_finalize_stmt(sqlite3_stmt *_Nullable *_Nonnull pstmt) {
  cql_contract(pstmt);
  if (*pstmt) {
    cql_sqlite3_finalize(*pstmt);
    *pstmt = NULL;
  }
}

// Read a nullable bool from the statement at the indicated index.
// If the column is null then return null.
// If not null then return the value.
// This is used in the general purpose column readers cql_multifetch and cql_multifetch_meta.
// to get column access to bools without having to open code the null check every time.
void cql_column_nullable_bool(
  sqlite3_stmt *_Nonnull stmt,
  cql_int32 index,
  cql_nullable_bool *_Nonnull data)
{
  if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
    cql_set_null(*data);
  }
  else {
    cql_set_notnull(*data, !!sqlite3_column_int(stmt, index));
  }
}

// Read a nullable int32 from the statement at the indicated index.
// If the column is null then return null.
// If not null then return the value.
// This is used in the general purpose column readers cql_multifetch and cql_multifetch_meta.
// to get column access to int32s without having to open code the null check every time.
void cql_column_nullable_int32(
  sqlite3_stmt *_Nonnull stmt,
  cql_int32 index,
  cql_nullable_int32 *_Nonnull data)
{
  if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
    cql_set_null(*data);
  }
  else {
    cql_set_notnull(*data, sqlite3_column_int(stmt, index));
  }
}

// Read a nullable int64 from the statement at the indicated index.
// If the column is null then return null.
// If not null then return the value.
// This is used in the general purpose column readers cql_multifetch and cql_multifetch_meta.
// to get column access to int64s without having to open code the null check every time.
void cql_column_nullable_int64(
  sqlite3_stmt *_Nonnull stmt,
  cql_int32 index,
  cql_nullable_int64 *_Nonnull data)
{
  if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
    cql_set_null(*data);
  }
  else {
    cql_set_notnull(*data, sqlite3_column_int64(stmt, index));
  }
}

// Read a nullable double from the statement at the indicated index.
// If the column is null then return null.
// If not null then return the value.
// This is used in the general purpose column readers cql_multifetch and cql_multifetch_meta.
// to get column access to doubles without having to open code the null check every time.
void cql_column_nullable_double(
  sqlite3_stmt *_Nonnull stmt,
  cql_int32 index,
  cql_nullable_double *_Nonnull data)
{
  if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
    cql_set_null(*data);
  }
  else {
    cql_set_notnull(*data, sqlite3_column_double(stmt, index));
  }
}

// Read a nullable string reference from the statement at the indicated index.
// If the column is null then return null.
// If not null then return the value.
// This is used in the general purpose column readers cql_multifetch and cql_multifetch_meta.
// to get column access to strings without having to open code the null check every time.
void cql_column_nullable_string_ref(
  sqlite3_stmt *_Nonnull stmt,
  cql_int32 index,
  cql_string_ref _Nullable *_Nonnull data)
{
  // the target may already have data, release it if it does
  cql_string_release(*data);
  if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
    *data = NULL;
  }
  else {
    *data = cql_string_ref_new((const char *)sqlite3_column_text(stmt, index));
  }
}

// Read a string reference from the statement at the indicated index.
// This is used in the general purpose column readers cql_multifetch and cql_multifetch_meta.
void cql_column_string_ref(
  sqlite3_stmt *_Nonnull stmt,
  cql_int32 index,
  cql_string_ref _Nonnull *_Nonnull data)
{
  // the target may already have data, release it if it does
  cql_string_release(*data);
  *data = cql_string_ref_new((const char *)sqlite3_column_text(stmt, index));
}

// Read a nullable blob reference from the statement at the indicated index.
// If the column is null then return null.
// If not null then return the value.
// This is used in the general purpose column readers cql_multifetch and cql_multifetch_meta.
// to get column access to blobs without having to open code the null check every time.
void cql_column_nullable_blob_ref(
  sqlite3_stmt *_Nonnull stmt,
  cql_int32 index,
  cql_blob_ref _Nullable *_Nonnull data)
{
  // the target may already have data, release it if it does
  cql_blob_release(*data);
  if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
    *data = NULL;
  }
  else {
    const void *bytes = sqlite3_column_blob(stmt, index);
    cql_uint32 size = sqlite3_column_bytes(stmt, index);
    *data = cql_blob_ref_new(bytes, size);
  }
}

// Read a blob reference from the statement at the indicated index.
// This is used in the general purpose column readers cql_multifetch and cql_multifetch_meta.
void cql_column_blob_ref(
  sqlite3_stmt *_Nonnull stmt,
  cql_int32 index,
  cql_blob_ref _Nonnull *_Nonnull data)
{
  // the target may already have data, release it if it does
  cql_blob_release(*data);
  const void *bytes = sqlite3_column_blob(stmt, index);
  cql_uint32 size = sqlite3_column_bytes(stmt, index);
  *data = cql_blob_ref_new(bytes, size);
}

// This helper is used by CQL to set an object reference.  It does the primitive retain/release operations.
// For now all the reference types are the same in this regard but there are different helpers for
// additional type safety in the generated code and readability (and breakpoints).
void cql_set_object_ref(
  cql_object_ref _Nullable *_Nonnull target,
  cql_object_ref _Nullable source)
{
  // upcount first in case source is an alias for target
  cql_object_retain(source);
  cql_object_release(*target);
  *target = source;
}

// This helper is used by CQL to set a string reference.  It does the primitive retain/release operations.
// For now all the reference types are the same in this regard but there are different helpers for
// additional type safety in the generated code and readability (and breakpoints).
void cql_set_string_ref(
  cql_string_ref _Nullable *_Nonnull target,
  cql_string_ref _Nullable source)
{
  // upcount first in case source is an alias for target
  cql_string_retain(source);
  cql_string_release(*target);
  *target = source;
}

// This helper is used by CQL to set a blob reference.  It does the primitive retain/release operations.
// For now all the reference types are the same in this regard but there are different helpers for
// additional type safety in the generated code and readability (and breakpoints).
void cql_set_blob_ref(
  cql_blob_ref _Nullable *_Nonnull target,
  cql_blob_ref _Nullable source)
{
  // upcount first in case source is an alias for target
  cql_blob_retain(source);
  cql_blob_release(*target);
  *target = source;
}

#ifdef CQL_RUN_TEST
jmp_buf *_Nullable cql_contract_argument_notnull_tripwire_jmp_buf;
#endif

// Wraps calls to `cql_tripwire` to allow us to longjmp, if required. This is
// called for both the argument itself and, in the case of an INOUT NOT NULL
// reference type argument, what the argument points to as well.
static void cql_contract_argument_notnull_tripwire(void *_Nullable ptr, cql_uint32 position) {
#ifdef CQL_RUN_TEST
  if (cql_contract_argument_notnull_tripwire_jmp_buf && !ptr) {
    longjmp(*cql_contract_argument_notnull_tripwire_jmp_buf, position);
  }
#endif
  cql_tripwire(ptr);
}

// This will be called in the case of an INOUT NOT NULL reference type argument
// to ensure that `argument` does not point to NULL. This function does not need
// per-position variants (as `DEFINE_ARGUMENT_AT_POSITION_N_MUST_NOT_BE_NULL`
// enables) as such a function will always be above this in the stack.
// `__attribute__((optnone))` is used to ensure we actually see this in stack
// traces and it doesn't get inlined or merged away.
CQL_OPT_NONE static void cql_inout_reference_type_notnull_argument_must_not_point_to_null(
  void *_Nullable *_Nonnull argument,
  cql_int32 position)
{
  cql_contract_argument_notnull_tripwire(*argument, position);
}

// This helps us generate variants of nonnull argument enforcement for each of
// the first eight arguments. As above, `__attribute__((optnone))` prevents
// these from getting inlined or merged.
#define DEFINE_ARGUMENT_AT_POSITION_N_MUST_NOT_BE_NULL(N) \
  CQL_OPT_NONE \
  static void cql_argument_at_position_ ## N ## _must_not_be_null(void *_Nullable argument, cql_bool inout_notnull) { \
   cql_contract_argument_notnull_tripwire(argument, N); \
    if (inout_notnull) { \
      cql_inout_reference_type_notnull_argument_must_not_point_to_null(argument, N); \
    } \
  }

DEFINE_ARGUMENT_AT_POSITION_N_MUST_NOT_BE_NULL(1);
DEFINE_ARGUMENT_AT_POSITION_N_MUST_NOT_BE_NULL(2);
DEFINE_ARGUMENT_AT_POSITION_N_MUST_NOT_BE_NULL(3);
DEFINE_ARGUMENT_AT_POSITION_N_MUST_NOT_BE_NULL(4);
DEFINE_ARGUMENT_AT_POSITION_N_MUST_NOT_BE_NULL(5);
DEFINE_ARGUMENT_AT_POSITION_N_MUST_NOT_BE_NULL(6);
DEFINE_ARGUMENT_AT_POSITION_N_MUST_NOT_BE_NULL(7);
DEFINE_ARGUMENT_AT_POSITION_N_MUST_NOT_BE_NULL(8);

CQL_OPT_NONE static void cql_argument_at_position_9_or_greater_must_not_be_null(
  void *_Nullable argument,
  cql_uint32 position,
  cql_bool deref)
{
  cql_contract_argument_notnull_tripwire(argument, position);
  if (deref) {
    cql_inout_reference_type_notnull_argument_must_not_point_to_null(argument, position);
  }
}

// Calls a position-specific function that will call `cql_tripwire(argument)`
// (and `cql_tripwire(*argument)` when `deref` is true, as in the case of `INOUT
// arg R NOT NULL`, where `R` is some reference type). This is done so that a
// maximally informative function name will appear in stack traces.
//
// NOTE: This function takes a `position` starting from 1 instead of an `index`
// starting from 0 so that, when someone is debugging a crash, `position` will
// line up with the name of the position-specific function and not cause
// confusion. Having the "first argument" be "position 1", as opposed to "index
// 0", seems to be the most intuitive. It also makes things a bit cleaner when
// performing a longjmp during testing (because jumping with 0 is
// indistinguishable from jumping with 1).
static void cql_contract_argument_notnull_with_optional_dereference_check(
  void *_Nullable argument,
  cql_uint32 position,
  cql_bool deref)
{
  switch (position) {
    case 1:
      return cql_argument_at_position_1_must_not_be_null(argument, deref);
    case 2:
      return cql_argument_at_position_2_must_not_be_null(argument, deref);
    case 3:
      return cql_argument_at_position_3_must_not_be_null(argument, deref);
    case 4:
      return cql_argument_at_position_4_must_not_be_null(argument, deref);
    case 5:
      return cql_argument_at_position_5_must_not_be_null(argument, deref);
    case 6:
      return cql_argument_at_position_6_must_not_be_null(argument, deref);
    case 7:
      return cql_argument_at_position_7_must_not_be_null(argument, deref);
    case 8:
      return cql_argument_at_position_8_must_not_be_null(argument, deref);
    default:
      return cql_argument_at_position_9_or_greater_must_not_be_null(argument, position, deref);
  }
}

void cql_contract_argument_notnull(void * _Nullable argument, cql_uint32 position) {
  cql_contract_argument_notnull_with_optional_dereference_check(argument, position, false);
}

void cql_contract_argument_notnull_when_dereferenced(void * _Nullable argument, cql_uint32 position) {
  cql_contract_argument_notnull_with_optional_dereference_check(argument, position, true);
}

// Creates a growable byte-buffer.  This code is used in the creation of the data blob for a result set.
// The buffer will double in size when it would otherwise overflow resulting in at most 2N data operations
// for N rows.
void cql_bytebuf_open(cql_bytebuf *_Nonnull b) {
  b->max = BYTEBUF_GROWTH_SIZE;
  b->ptr = malloc(b->max);
  b->used = 0;
}

// Dispenses the buffer's memory when it is closed.
void cql_bytebuf_close(cql_bytebuf *_Nonnull b) {
  free(b->ptr);
  b->max = 0;
  b->ptr = NULL;
}

// Get more memory from the byte buffer.  This will be used to get memory for each new row
// in a result set.
// Note: the data is assumed to be location independent and reference count invariant.
// (i.e. you can memcpy it safely if you then also destroy the old copy)
void *_Nonnull cql_bytebuf_alloc(cql_bytebuf *_Nonnull b, int needed) {
  int32_t avail = b->max - b->used;

  if (needed > avail) {
    if (b->max > BYTEBUF_EXP_GROWTH_CAP) {
      b->max = needed + BYTEBUF_GROWTH_SIZE_AFTER_CAP + b->max;
    } else {
      b->max = needed + 2 * b->max;
    }
    char *newptr = malloc(b->max);

    memcpy(newptr, b->ptr, b->used);
    free(b->ptr);
    b->ptr = newptr;
  }

  void *result = b->ptr + b->used;
  b->used += needed;
  return result;
}

// simple helper to append into a byte buffer
void cql_bytebuf_append(cql_bytebuf *_Nonnull buffer, const void *_Nonnull data, int32_t bytes) {
  void *pv = cql_bytebuf_alloc(buffer, bytes);
  memcpy(pv, data, bytes);
}

// This is a simple wrapper on vsnprintf, we do two passes first to compute the bytes needed
// which we allocate using cql_bytebuf_alloc and then we write the formatted string.  Note that
// it's normal to call this many times or in mixed ways so the null terminator is not desired.
// The buffer gets the text of the string only.  Use cql_bytebuf_append_null to null terminate.
static void cql_vbprintf(cql_bytebuf *_Nonnull buffer, const char *_Nonnull format, va_list *_Nonnull args) {
  va_list pass1, pass2;
  va_copy(pass1, *args);
  va_copy(pass2, *args);

  // +1 to include the trailing null we will need (but don't want)
  uint32_t needed = (uint32_t)vsnprintf(NULL, 0, format, pass1) + 1;

  char *newptr = cql_bytebuf_alloc(buffer, needed);

  // We can't stop this from writing a null terminator
  vsnprintf(newptr, needed, format, pass2);

  // We don't want the null terminator, se we remove it.
  buffer->used--;

  va_end(pass1);
  va_end(pass2);
}

// This allows you to write into a bytebuf using a format string and varargs
// All the work is delegated really, vsnprinf ultimately does everything but
// first we need to call the function that does the size computation.
void cql_bprintf(cql_bytebuf *_Nonnull buffer, const char *_Nonnull format, ...) {
 va_list args;
 va_start(args, format);
 cql_vbprintf(buffer, format, &args);
 va_end(args);
}

// After using cql_bprintf it's pretty normal to need to add a null terminator
// to create a C style string.  Though not always depending on where the buffer is going.
// This helps with that need.
void cql_bytebuf_append_null(cql_bytebuf *_Nonnull buffer) {
  char var = 0;
  cql_bytebuf_append(buffer, &var, sizeof(var));
}

// If there is no row available we can use this helper to ensure that
// the output data is put into a known state.
static void cql_multinull(cql_int32 count, va_list *_Nonnull args) {
  for (cql_int32 column = 0; column < count; column++) {
    cql_int32 type = va_arg(*args, cql_int32);
    cql_int32 core_data_type = CQL_CORE_DATA_TYPE_OF(type);

    if (type & CQL_DATA_TYPE_NOT_NULL) {
      switch (core_data_type) {
        case CQL_DATA_TYPE_INT32: {
          cql_int32 *int32_data = va_arg(*args, cql_int32 *);
          *int32_data = 0;
          break;
        }
        case CQL_DATA_TYPE_INT64: {
          cql_int64 *int64_data = va_arg(*args, cql_int64 *);
          *int64_data = 0;
          break;
        }
        case CQL_DATA_TYPE_DOUBLE: {
          cql_double *double_data = va_arg(*args, cql_double *);
          *double_data = 0;
          break;
        }
        case CQL_DATA_TYPE_BOOL: {
          cql_bool *bool_data = va_arg(*args, cql_bool *);
          *bool_data = 0;
          break;
        }
        case CQL_DATA_TYPE_STRING: {
          cql_string_ref *str_ref = va_arg(*args, cql_string_ref *);
          cql_set_string_ref(str_ref, NULL);
          break;
        }
        case CQL_DATA_TYPE_BLOB: {
          cql_blob_ref *blob_ref = va_arg(*args, cql_blob_ref *);
          cql_set_blob_ref(blob_ref, NULL);
          break;
        }
      }
    }
    else {
      switch (core_data_type) {
        case CQL_DATA_TYPE_INT32: {
          cql_nullable_int32 *_Nonnull int32p = va_arg(*args, cql_nullable_int32 *_Nonnull);
          cql_set_null(*int32p);
          break;
        }
        case CQL_DATA_TYPE_INT64: {
          cql_nullable_int64 *_Nonnull int64p = va_arg(*args, cql_nullable_int64 *_Nonnull);
          cql_set_null(*int64p);
          break;
        }
        case CQL_DATA_TYPE_DOUBLE: {
          cql_nullable_double *_Nonnull doublep = va_arg(*args, cql_nullable_double *_Nonnull);
          cql_set_null(*doublep);
          break;
        }
        case CQL_DATA_TYPE_BOOL: {
          cql_nullable_bool *_Nonnull boolp = va_arg(*args, cql_nullable_bool *_Nonnull);
          cql_set_null(*boolp);
          break;
        }
        case CQL_DATA_TYPE_STRING: {
          cql_string_ref *str_ref = va_arg(*args, cql_string_ref *);
          cql_set_string_ref(str_ref, NULL);
          break;
        }
        case CQL_DATA_TYPE_BLOB: {
          cql_blob_ref *blob_ref = va_arg(*args, cql_blob_ref *);
          cql_set_blob_ref(blob_ref, NULL);
          break;
        }
      }
    }
  }
}

// This helper fetch a column value from sqlite and store it in the holder.
// CQL encode column value with flag CQL_DATA_TYPE_ENCODED as soon as they're
// read from db like that only encoded value is accessible. This means the
// proc creating result_set should decode explicitely sensitive column value
// to get the real value.
static void cql_fetch_field(
    cql_int32 type,
    cql_int32 column,
    sqlite3 *_Nonnull db,
    sqlite3_stmt *_Nullable stmt,
    char *_Nonnull field,
    cql_bool enable_encoding,
    cql_int32 encode_context_type,
    char *_Nullable encode_context_field,
    cql_object_ref _Nullable encoder)
{
  bool is_encoded = (type & CQL_DATA_TYPE_ENCODED) && enable_encoding;
  cql_int32 core_data_type_and_not_null = type & ~CQL_DATA_TYPE_ENCODED;

  switch (core_data_type_and_not_null) {
    case CQL_DATA_TYPE_INT32 | CQL_DATA_TYPE_NOT_NULL: {
      cql_int32 *int32_data = (cql_int32 *)field;
      *int32_data = sqlite3_column_int(stmt, column);
      if (is_encoded) {
        *int32_data = cql_encode_int32(encoder, *int32_data, encode_context_type, encode_context_field);
      }
      break;
    }
    case CQL_DATA_TYPE_INT64 | CQL_DATA_TYPE_NOT_NULL: {
      cql_int64 *int64_data = (cql_int64 *)field;
      *int64_data = sqlite3_column_int64(stmt, column);
      if (is_encoded) {
        *int64_data = cql_encode_int64(encoder, *int64_data, encode_context_type, encode_context_field);
      }
      break;
    }
    case CQL_DATA_TYPE_DOUBLE | CQL_DATA_TYPE_NOT_NULL: {
      cql_double *double_data = (cql_double *)field;
      *double_data = sqlite3_column_double(stmt, column);
      if (is_encoded) {
        *double_data = cql_encode_double(encoder, *double_data, encode_context_type, encode_context_field);
      }
      break;
    }
    case CQL_DATA_TYPE_BOOL | CQL_DATA_TYPE_NOT_NULL: {
      cql_bool *bool_data = (cql_bool *)field;
      *bool_data = !!sqlite3_column_int(stmt, column);
      if (is_encoded) {
        *bool_data = cql_encode_bool(encoder, *bool_data, encode_context_type, encode_context_field);
      }
      break;
    }
    case CQL_DATA_TYPE_STRING | CQL_DATA_TYPE_NOT_NULL: {
      cql_string_ref *str_ref = (cql_string_ref *)field;
      cql_column_string_ref(stmt, column, str_ref);
      if (is_encoded) {
        cql_string_ref new_str_ref = cql_encode_string_ref_new(encoder, *str_ref, encode_context_type, encode_context_field);
        cql_set_string_ref(str_ref, new_str_ref);
        cql_string_release(new_str_ref);
      }
      break;
    }
    case CQL_DATA_TYPE_BLOB | CQL_DATA_TYPE_NOT_NULL: {
      cql_blob_ref *blob_ref = (cql_blob_ref *)field;
      cql_column_blob_ref(stmt, column, blob_ref);
      if (is_encoded) {
        cql_blob_ref new_blob_ref = cql_encode_blob_ref_new(encoder, *blob_ref, encode_context_type, encode_context_field);
        cql_set_blob_ref(blob_ref, new_blob_ref);
        cql_blob_release(new_blob_ref);
      }
      break;
    }
    case CQL_DATA_TYPE_INT32: {
      cql_nullable_int32 *_Nonnull int32p = (cql_nullable_int32 *)field;
      cql_column_nullable_int32(stmt, column, int32p);
      if (is_encoded && !int32p->is_null) {
        int32p->value = cql_encode_int32(encoder, int32p->value, encode_context_type, encode_context_field);
      }
      break;
    }
    case CQL_DATA_TYPE_INT64: {
      cql_nullable_int64 *_Nonnull int64p = (cql_nullable_int64 *)field;
      cql_column_nullable_int64(stmt, column, int64p);
      if (is_encoded && !int64p->is_null) {
        int64p->value = cql_encode_int64(encoder, int64p->value, encode_context_type, encode_context_field);
      }
      break;
    }
    case CQL_DATA_TYPE_DOUBLE: {
      cql_nullable_double *_Nonnull doublep = (cql_nullable_double *)field;
      cql_column_nullable_double(stmt, column, doublep);
      if (is_encoded && !doublep->is_null) {
        doublep->value = cql_encode_double(encoder, doublep->value, encode_context_type, encode_context_field);
      }
      break;
    }
    case CQL_DATA_TYPE_BOOL: {
      cql_nullable_bool *_Nonnull boolp = (cql_nullable_bool *)field;
      cql_column_nullable_bool(stmt, column, boolp);
      if (is_encoded && !boolp->is_null) {
        boolp->value = cql_encode_bool(encoder, boolp->value, encode_context_type, encode_context_field);
      }
      break;
    }
    case CQL_DATA_TYPE_STRING: {
      cql_string_ref *str_ref = (cql_string_ref *)field;
      cql_column_nullable_string_ref(stmt, column, str_ref);
      if (is_encoded && *str_ref) {
        cql_string_ref new_str_ref = cql_encode_string_ref_new(encoder, *str_ref, encode_context_type, encode_context_field);
        cql_set_string_ref(str_ref, new_str_ref);
        cql_string_release(new_str_ref);
      }
      break;
    }
    case CQL_DATA_TYPE_BLOB: {
      cql_blob_ref *blob_ref = (cql_blob_ref *)field;
      cql_column_nullable_blob_ref(stmt, column, blob_ref);
      if (is_encoded && *blob_ref) {
        cql_blob_ref new_blob_ref = cql_encode_blob_ref_new(encoder, *blob_ref, encode_context_type, encode_context_field);
        cql_set_blob_ref(blob_ref, new_blob_ref);
        cql_blob_release(new_blob_ref);
      }
      break;
    }
  }
}

// This method lets us get lots of columns out of a statement with one call
// in the generated code saving us a lot of error management and reducing the
// generated code cost to just the offsets and types.  This version does
// the fetch based on the "fetch info" which includes, among other things
// an array of types and an array of offsets.
void cql_multifetch_meta(char *_Nonnull data, cql_fetch_info *_Nonnull info) {
  cql_contract(info->stmt);
  cql_contract(info->db);
  sqlite3_stmt *stmt = info->stmt;
  sqlite3 *db = info->db;
  uint8_t *_Nonnull data_types = info->data_types;
  uint16_t *_Nonnull col_offsets = info->col_offsets;

  uint32_t count = col_offsets[0];
  col_offsets++;

  // If vault context column is specified, we fetch it first with the column index
  cql_int32 encode_context_type = -1;
  char *encode_context_field = NULL;
  if (info->encode_context_index >= 0) {
    encode_context_type = data_types[info->encode_context_index];
    encode_context_field = data + col_offsets[info->encode_context_index];
    cql_fetch_field(encode_context_type,
                    info->encode_context_index,
                    db,
                    stmt,
                    encode_context_field,
                    false /* enable_encoding */,
                    -1 /* encode_context_type */,
                    NULL /* encode_context_field */,
                    info->encoder);
  }

  for (cql_int32 column = 0; column < count; column++) {
    if (column == info->encode_context_index) {
      // This vault context column has been fetched already, skip
      continue;
    }
    uint8_t type = data_types[column];
    char *field = data + col_offsets[column];
    // We're fetching column values from db to store in a result_set. Therefore we
    // need to encode those values because it's the result_set output of the proc.
    // Because of that we set enable_encoding = TRUE. The value true means if the
    // field has the CQL_DATA_TYPE_ENCODED bit then encode otherwise don't
    cql_fetch_field(type,
                    column,
                    db,
                    stmt,
                    field,
                    true /* enable_encoding */,
                    encode_context_type,
                    encode_context_field,
                    info->encoder);
  }
}

// This method lets us get lots of columns out of a statement with one call
// in the generated code saving us a lot of error management and reducing the
// generated code cost to just the offsets and types.  This version does the
// fetching using varargs with types and addresses.  This is the most flexible
// as it allows writing into local variables and out parameters.
void cql_multifetch(cql_code rc, sqlite3_stmt *_Nullable stmt, cql_int32 count, ...) {
  va_list args;
  va_start(args, count);

  if (rc != SQLITE_ROW) {
    cql_multinull(count, &args);
    va_end(args);
    return;
  }

  cql_contract(stmt);
  sqlite3 *db = sqlite3_db_handle(stmt);

  for (cql_int32 column = 0; column < count; column++) {
    cql_int32 type = va_arg(args, cql_int32);
    void *field = va_arg(args, void *);
    // We're fetching column values from db to store in variable. Therefore we
    // don't need to encode it because it's not a result_set output of the proc.
    // Because of that we set enable_encoding = FALSE. The value false means
    // do not encode even if the field has the CQL_DATA_TYPE_ENCODED bit.
    cql_fetch_field(type,
                    column,
                    db,
                    stmt,
                    field,
                    false /* enable_encoding */,
                    -1 /* encode_context_type */,
                    NULL /* encode_context_field */,
                    NULL /* encoder */);
  }

  va_end(args);
}

// This method lets us get lots of columns out of a statement with one call
// in the generated code saving us a lot of error management and reducing the
// generated code cost to just the offsets and types.  This version does the
// fetching using varargs with types and addresses.  This is the most flexible
// as it allows writing into local variables and out parameters.
void cql_copyoutrow(
  sqlite3 *_Nullable db,
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 count, ...)
{
  cql_contract(result_set);

  va_list args;
  va_start(args, count);

  cql_int32 row_count = cql_result_set_get_count(result_set);

  if (row >= row_count || row < 0) {
    cql_multinull(count, &args);
    va_end(args);
    return;
  }

  bool got_decoder = false;
  cql_object_ref _Nullable encoder = NULL;

  // Find vault context column
  cql_result_set_meta *meta = cql_result_set_get_meta(result_set);
  cql_int32 encode_context_type = -1;
  char *encode_context_field = NULL;
  if (meta->encodeContextIndex >= 0) {
    encode_context_type = meta->dataTypes[meta->encodeContextIndex];
    encode_context_field = cql_address_of_col(result_set, row, meta->encodeContextIndex, &encode_context_type);
  }
  // sometimes the below usages resolve to do-noting macros and thus this gets
  // considered as unused. So to always give it a "usage" cast it to void here.
  (void)encode_context_field;

  for (cql_int32 column = 0; column < count; column++) {
    cql_int32 type = va_arg(args, cql_int32);
    cql_int32 core_data_type_and_not_null = CQL_CORE_DATA_TYPE_OF(type) | (type & CQL_DATA_TYPE_NOT_NULL);
    // This is important to document should_decode because it needs some clarification
    // that impact how the vault feature work in CQL.
    // This function copy raw values from a result set (out union). Therefore if we
    // detect that some of the fields read are encoded (should_decode == TRUE), then we
    // have to decode the value copied.
    // We never encode values read from result_set even though the type flag
    // is CQL_DATA_TYPE_ENCODED. The flag CQL_DATA_TYPE_ENCODED is used to encode fields
    // read from db (see cql_multifetch(...)) or to decode fields read from result_set (out union).
    bool should_decode = db && cql_result_set_get_is_encoded_col(result_set, column);

    if (should_decode && !got_decoder) {
      encoder = cql_copy_encoder(db);
      got_decoder = true;  // note the decoder might be null so we need a flag
    }

    switch (core_data_type_and_not_null) {
      case CQL_DATA_TYPE_INT32 | CQL_DATA_TYPE_NOT_NULL: {
        cql_int32 *int32_data = va_arg(args, cql_int32 *);
        *int32_data = cql_result_set_get_int32_col(result_set, row, column);
        if (should_decode) {
          *int32_data = cql_decode_int32(encoder, *int32_data, encode_context_type, encode_context_field);
        }
        break;
      }
      case CQL_DATA_TYPE_INT64 | CQL_DATA_TYPE_NOT_NULL: {
        cql_int64 *int64_data = va_arg(args, cql_int64 *);
        *int64_data = cql_result_set_get_int64_col(result_set, row, column);
        if (should_decode) {
          *int64_data = cql_decode_int64(encoder, *int64_data, encode_context_type, encode_context_field);
        }
        break;
      }
      case CQL_DATA_TYPE_DOUBLE | CQL_DATA_TYPE_NOT_NULL: {
        cql_double *double_data = va_arg(args, cql_double *);
        *double_data = cql_result_set_get_double_col(result_set, row, column);
        if (should_decode) {
          *double_data = cql_decode_double(encoder, *double_data, encode_context_type, encode_context_field);
        }
        break;
      }
      case CQL_DATA_TYPE_BOOL | CQL_DATA_TYPE_NOT_NULL: {
        cql_bool *bool_data = va_arg(args, cql_bool *);
        *bool_data = cql_result_set_get_bool_col(result_set, row, column);
        if (should_decode) {
          *bool_data = cql_decode_bool(encoder, *bool_data, encode_context_type, encode_context_field);
        }
        break;
      }
      case CQL_DATA_TYPE_STRING | CQL_DATA_TYPE_NOT_NULL: {
        cql_string_ref *str_ref = va_arg(args, cql_string_ref *);
        cql_set_string_ref(str_ref, cql_result_set_get_string_col(result_set, row, column));
        cql_string_ref new_str_ref = NULL;
        if (should_decode) {
          new_str_ref = cql_decode_string_ref_new(encoder, *str_ref, encode_context_type, encode_context_field);
          cql_set_string_ref(str_ref, new_str_ref);
          cql_string_release(new_str_ref);
        }
        break;
      }
      case CQL_DATA_TYPE_BLOB | CQL_DATA_TYPE_NOT_NULL: {
        cql_blob_ref *blob_ref = va_arg(args, cql_blob_ref *);
        cql_set_blob_ref(blob_ref, cql_result_set_get_blob_col(result_set, row, column));
        cql_blob_ref new_blob_ref = NULL;
        if (should_decode) {
          new_blob_ref = cql_decode_blob_ref_new(encoder, *blob_ref, encode_context_type, encode_context_field);
          cql_set_blob_ref(blob_ref, new_blob_ref);
          cql_blob_release(new_blob_ref);
        }
        break;
      }
      case CQL_DATA_TYPE_OBJECT | CQL_DATA_TYPE_NOT_NULL: {
        cql_object_ref *obj_ref = va_arg(args, cql_object_ref *);
        cql_set_object_ref(obj_ref, cql_result_set_get_object_col(result_set, row, column));
        break;
      }
      case CQL_DATA_TYPE_INT32: {
        cql_nullable_int32 *_Nonnull int32p = va_arg(args, cql_nullable_int32 *_Nonnull);
        if (cql_result_set_get_is_null_col(result_set, row, column)) {
          cql_set_null(*int32p);
        }
        else {
          cql_set_notnull(*int32p, cql_result_set_get_int32_col(result_set, row, column));
        }
        if (!int32p->is_null && should_decode) {
          int32p->value = cql_decode_int32(encoder, int32p->value, encode_context_type, encode_context_field);
        }
        break;
      }
      case CQL_DATA_TYPE_INT64: {
        cql_nullable_int64 *_Nonnull int64p = va_arg(args, cql_nullable_int64 *_Nonnull);
        if (cql_result_set_get_is_null_col(result_set, row, column)) {
          cql_set_null(*int64p);
        }
        else {
          cql_set_notnull(*int64p, cql_result_set_get_int64_col(result_set, row, column));
        }
        if (!int64p->is_null && should_decode) {
          int64p->value = cql_decode_int64(encoder, int64p->value, encode_context_type, encode_context_field);
        }
        break;
      }
      case CQL_DATA_TYPE_DOUBLE: {
        cql_nullable_double *_Nonnull doublep = va_arg(args, cql_nullable_double *_Nonnull);
        if (cql_result_set_get_is_null_col(result_set, row, column)) {
          cql_set_null(*doublep);
        }
        else {
          cql_set_notnull(*doublep, cql_result_set_get_double_col(result_set, row, column));
        }
        if (!doublep->is_null && should_decode) {
          doublep->value = cql_decode_double(encoder, doublep->value, encode_context_type, encode_context_field);
        }
        break;
      }
      case CQL_DATA_TYPE_BOOL: {
        cql_nullable_bool *_Nonnull boolp = va_arg(args, cql_nullable_bool *_Nonnull);
        if (cql_result_set_get_is_null_col(result_set, row, column)) {
          cql_set_null(*boolp);
        }
        else {
          cql_set_notnull(*boolp, cql_result_set_get_bool_col(result_set, row, column));
        }
        if (!boolp->is_null && should_decode) {
          boolp->value = cql_decode_bool(encoder, boolp->value, encode_context_type, encode_context_field);
        }
        break;
      }
      case CQL_DATA_TYPE_STRING: {
        cql_string_ref *str_ref = va_arg(args, cql_string_ref *);
        cql_set_string_ref(str_ref, cql_result_set_get_string_col(result_set, row, column));
        cql_string_ref new_str_ref = NULL;
        if (*str_ref && should_decode) {
          new_str_ref = cql_decode_string_ref_new(encoder, *str_ref, encode_context_type, encode_context_field);
          cql_set_string_ref(str_ref, new_str_ref);
          cql_string_release(new_str_ref);
        }
        break;
      }
      case CQL_DATA_TYPE_BLOB: {
        cql_blob_ref *blob_ref = va_arg(args, cql_blob_ref *);
        cql_set_blob_ref(blob_ref, cql_result_set_get_blob_col(result_set, row, column));
        cql_blob_ref new_blob_ref = NULL;
        if (*blob_ref && should_decode) {
          new_blob_ref = cql_decode_blob_ref_new(encoder, *blob_ref, encode_context_type, encode_context_field);
          cql_set_blob_ref(blob_ref, new_blob_ref);
          cql_blob_release(new_blob_ref);
        }
        break;
      }
      case CQL_DATA_TYPE_OBJECT: {
        cql_object_ref *obj_ref = va_arg(args, cql_object_ref *);
        cql_set_object_ref(obj_ref, cql_result_set_get_object_col(result_set, row, column));
        break;
      }
    }
  }

  cql_object_release(encoder);

  va_end(args);
}

// This is just the helper to ignore the indicated arg
// because the predicates array tell us it is to be skipped
static void cql_skip_arg(cql_int32 type, va_list *_Nonnull args)
{
  cql_int32 core_data_type = CQL_CORE_DATA_TYPE_OF(type);

  if (type & CQL_DATA_TYPE_NOT_NULL) {
    switch (core_data_type) {
      case CQL_DATA_TYPE_INT32:
        (void)va_arg(*args, cql_int32);
        break;
      case CQL_DATA_TYPE_INT64:
        (void)va_arg(*args, cql_int64);
        break;
      case CQL_DATA_TYPE_DOUBLE:
        (void)va_arg(*args, cql_double);
        break;
      case CQL_DATA_TYPE_BOOL:
        (void)va_arg(*args, cql_int32);
        break;
      case CQL_DATA_TYPE_STRING:
        (void)va_arg(*args, cql_string_ref);
        break;
      case CQL_DATA_TYPE_BLOB:
        (void)va_arg(*args, cql_blob_ref);
        break;
      case CQL_DATA_TYPE_OBJECT:
        (void)va_arg(*args, cql_object_ref);
        break;
    }
  }
  else {
    switch (core_data_type) {
      case CQL_DATA_TYPE_INT32:
        (void)va_arg(*args, const cql_nullable_int32 *_Nonnull);
        break;
      case CQL_DATA_TYPE_INT64:
        (void)va_arg(*args, const cql_nullable_int64 *_Nonnull);
        break;
      case CQL_DATA_TYPE_DOUBLE:
        (void)va_arg(*args, const cql_nullable_double *_Nonnull);
        break;
      case CQL_DATA_TYPE_BOOL:
        (void)va_arg(*args, const cql_nullable_bool *_Nonnull);
        break;
      case CQL_DATA_TYPE_STRING:
        (void)va_arg(*args, cql_string_ref);
        break;
      case CQL_DATA_TYPE_BLOB:
        (void)va_arg(*args, cql_blob_ref);
        break;
      case CQL_DATA_TYPE_OBJECT:
        (void)va_arg(*args, cql_object_ref);
        break;
    }
  }
}

// This helper lets us bind many variables to a statement with one call.  The
// resulting code gen can be a lot smaller as there is only the one error check
// needed and you need only provide the values to bind and the offsets for
// each of the variables.  The resulting code is much more economical.
static void cql_multibind_v(
  cql_code *_Nonnull prc,
  sqlite3 *_Nonnull db,
  sqlite3_stmt *_Nullable *_Nonnull pstmt,
  cql_int32 count,
  const char *_Nullable vpreds,
  va_list *_Nonnull args)
{
  cql_int32 column = 1;

  for (cql_int32 i = 0; *prc == SQLITE_OK && i < count; i++) {
    cql_contract(pstmt && *pstmt);
    cql_int32 type = va_arg(*args, cql_int32);
    cql_int32 core_data_type = CQL_CORE_DATA_TYPE_OF(type);

    if (vpreds && !vpreds[i]) {
      cql_skip_arg(type, args);
      continue;
    }

    if (type & CQL_DATA_TYPE_NOT_NULL) {
      switch (core_data_type) {
        case CQL_DATA_TYPE_INT32: {
          cql_int32 int32_data = va_arg(*args, cql_int32);
          *prc = sqlite3_bind_int(*pstmt, column, int32_data);
          column++;
          break;
        }
        case CQL_DATA_TYPE_INT64: {
          cql_int64 int64_data = va_arg(*args, cql_int64);
          *prc = sqlite3_bind_int64(*pstmt, column, int64_data);
          column++;
          break;
        }
        case CQL_DATA_TYPE_DOUBLE: {
          cql_double double_data = va_arg(*args, cql_double);
          *prc = sqlite3_bind_double(*pstmt, column, double_data);
          column++;
          break;
        }
        case CQL_DATA_TYPE_BOOL: {
          cql_bool bool_data = !!(cql_bool)va_arg(*args, cql_int32);
          *prc = sqlite3_bind_int(*pstmt, column, bool_data);
          column++;
          break;
        }
        case CQL_DATA_TYPE_STRING: {
          cql_string_ref str_ref = va_arg(*args, cql_string_ref);
          cql_alloc_cstr(temp, str_ref);
          *prc = sqlite3_bind_text(*pstmt, column, temp, -1, SQLITE_TRANSIENT);
          cql_free_cstr(temp, str_ref);
          column++;
          break;
        }
        case CQL_DATA_TYPE_BLOB: {
          cql_blob_ref blob_ref = va_arg(*args, cql_blob_ref);
          const void *bytes = cql_get_blob_bytes(blob_ref);
          cql_uint32 size = cql_get_blob_size(blob_ref);
          *prc = sqlite3_bind_blob(*pstmt, column, bytes, size, SQLITE_TRANSIENT);
          column++;
          break;
        }
        case CQL_DATA_TYPE_OBJECT: {
          cql_object_ref obj_ref = va_arg(*args, cql_object_ref);
          *prc = sqlite3_bind_int64(*pstmt, column, (int64_t)obj_ref);
          column++;
          break;
        }
      }
    }
    else {
      switch (core_data_type) {
        case CQL_DATA_TYPE_INT32: {
          const cql_nullable_int32 *_Nonnull int32p = va_arg(*args, const cql_nullable_int32 *_Nonnull);
          *prc = int32p->is_null ? sqlite3_bind_null(*pstmt, column) :
                                   sqlite3_bind_int(*pstmt, column, int32p->value);
          column++;
          break;
        }
        case CQL_DATA_TYPE_INT64: {
          const cql_nullable_int64 *_Nonnull int64p = va_arg(*args, const cql_nullable_int64 *_Nonnull);
          *prc =int64p->is_null ? sqlite3_bind_null(*pstmt, column) :
                                  sqlite3_bind_int64(*pstmt, column, int64p->value);
          column++;
          break;
        }
        case CQL_DATA_TYPE_DOUBLE: {
          const cql_nullable_double *_Nonnull doublep = va_arg(*args, const cql_nullable_double *_Nonnull);
          *prc = doublep->is_null ? sqlite3_bind_null(*pstmt, column) :
                                    sqlite3_bind_double(*pstmt, column, doublep->value);
          column++;
          break;
        }
        case CQL_DATA_TYPE_BOOL: {
          const cql_nullable_bool *_Nonnull boolp = va_arg(*args, const cql_nullable_bool *_Nonnull);
          *prc =boolp->is_null ? sqlite3_bind_null(*pstmt, column) :
                                 sqlite3_bind_int(*pstmt, column, !!boolp->value);
          column++;
          break;
        }
        case CQL_DATA_TYPE_STRING: {
          cql_string_ref _Nullable nullable_str_ref = va_arg(*args, cql_string_ref);
          if (!nullable_str_ref) {
            *prc = sqlite3_bind_null(*pstmt, column);
          }
          else {
            cql_alloc_cstr(temp, nullable_str_ref);
            *prc = sqlite3_bind_text(*pstmt, column, temp, -1, SQLITE_TRANSIENT);
            cql_free_cstr(temp, nullable_str_ref);
          }
          column++;
          break;
        }
        case CQL_DATA_TYPE_BLOB: {
          cql_blob_ref _Nullable nullable_blob_ref = va_arg(*args, cql_blob_ref);
          if (!nullable_blob_ref) {
            *prc = sqlite3_bind_null(*pstmt, column);
          }
          else {
            const void *bytes = cql_get_blob_bytes(nullable_blob_ref);
            cql_uint32 size = cql_get_blob_size(nullable_blob_ref);
            *prc = sqlite3_bind_blob(*pstmt, column, bytes, size, SQLITE_TRANSIENT);
          }
          column++;
          break;
        }
        case CQL_DATA_TYPE_OBJECT: {
          cql_object_ref _Nullable nullable_obj_ref = va_arg(*args, cql_object_ref);
          *prc = sqlite3_bind_int64(*pstmt, column, (int64_t)nullable_obj_ref);
          column++;
          break;
        }
      }
    }
    cql_finalize_on_error(*prc, pstmt);
  }
}

// This wraps the underlying varargs worker, with no variable predicates
void cql_multibind(
  cql_code *_Nonnull prc,
  sqlite3 *_Nonnull db,
  sqlite3_stmt *_Nullable *_Nonnull pstmt,
  cql_int32 count, ...)
{
  va_list args;
  va_start(args, count);
  cql_multibind_v(prc, db, pstmt, count, NULL, &args);
  va_end(args);
}

// This wraps the underlying varargs worker, with variable predicates
void cql_multibind_var(
  cql_code *_Nonnull prc,
  sqlite3 *_Nonnull db,
  sqlite3_stmt *_Nullable *_Nonnull pstmt,
  cql_int32 count,
  const char *_Nullable vpreds, ...)
{
  va_list args;
  va_start(args, vpreds);
  cql_multibind_v(prc, db, pstmt, count, vpreds, &args);
  va_end(args);
}

// In a single row of a result set or a single auto-cursor, release all the references in that row
// Note that all the references are together and they begin at refs_offset.
void cql_release_offsets(void *_Nonnull pv, cql_uint16 refs_count, cql_uint16 refs_offset) {
  if (refs_count) {
    // first entry in the array is the count
    char *base = pv;

    // each entry then tells us the offset of an embedded pointer
    for (cql_int32 i = 0; i < refs_count; i++) {
      cql_release(*(cql_type_ref *)(base + refs_offset));
      *(cql_type_ref *)(base + refs_offset) = NULL;
      refs_offset += sizeof(cql_type_ref);
    }
  }
}

// In a single row of a result set or a single auto-cursor, retain all the references in that row
// Note that all the references are together and they begin at refs_offset.
void cql_retain_offsets(void *_Nonnull pv, cql_uint16 refs_count, cql_uint16 refs_offset) {
  if (refs_count) {
    char *base = pv;

    // each entry then tells us the offset of an embedded pointer
    for (cql_int32 i = 0; i < refs_count; i++) {
      cql_retain(*(cql_type_ref *)(base + refs_offset));
      refs_offset += sizeof(cql_type_ref);
    }
  }
}

// Teardown an entire result set by iterating the rows and then releasing
// all of the references in each row using cql_release_offsets.  Once that
// is done, it's safe to free the entire blob of storage.
void cql_result_set_teardown(cql_result_set_ref _Nonnull result_set) {
  cql_result_set_meta *meta = cql_result_set_get_meta(result_set);
  size_t row_size = meta->rowsize;
  cql_int32 count = cql_result_set_get_count(result_set);
  cql_uint16 refs_count = meta->refsCount;
  cql_uint16 refs_offset = meta->refsOffset;
  char *_Nullable data = (char *)cql_result_set_get_data(result_set);
  char *_Nullable row = data;

  if (refs_count && count) {
    for (cql_int32 i = 0; i < count; i++) {
      cql_release_offsets(row, refs_count, refs_offset);
      row += row_size;
    }
  }

  free(data);
}

// Record the desired user-teardown function
void cql_result_set_set_custom_teardown(
  cql_result_set_ref _Nonnull result_set,
  void(*_Nonnull custom_teardown)(cql_result_set_ref _Nonnull result_set)) {
  cql_result_set_meta *meta = cql_result_set_get_meta(result_set);
  meta->custom_teardown = custom_teardown;
}

// Hash a cursor or row as described by the buffer size and refs offset
static cql_hash_code cql_hash_buffer(
  const char *_Nonnull data,
  size_t row_size,
  cql_uint16 refs_count,
  cql_uint16 refs_offset)
{
  // we'll do a normal hash on everything up to the first reference type
  // note: the refs are all guaranteed to be at the end AND the padding
  // is guaranteed to be zero-filled.  These are important invariants that
  // let us do a much simpler/faster/smaller hash.
  size_t size = row_size;
  if (refs_count) {
    size = refs_offset;
  }

  // Note that we hash even pad bytes because we always fully clear rows
  // before set set them to anything so any pad bytes are known to be 0
  // and hence will not randomize the hash (but they will change it).
  cql_hash_code hash = 0;
  unsigned char *bytes = (unsigned char *)data;
  hash = 5381;   // djb2
  while (size--) {
    hash = ((hash << 5) + hash) + *bytes++; /* hash * 33 + c */
  }

  if (refs_count) {
    // first entry is the count, then there are count more entries hence loop <= count
    for (int32_t i = 0; i < refs_count; i++) {
      cql_hash_code ref_hash = cql_ref_hash(*(cql_type_ref *)(data + refs_offset));
      hash = ((hash << 5) + hash) + ref_hash;
      refs_offset += sizeof(cql_type_ref);
    }
  }

  return hash;
}

// Hash the indicated row using a general purpose hash method and the reference
// type hashers.
// * the non-reference data is at the start of the row until the refs_offset
// * the references follow and there are refs_count of them.
// * these values are available in the metadata
// This single function can hash any row of any result set, thereby saving a lot
// of code generation.
cql_hash_code cql_row_hash(cql_result_set_ref _Nonnull result_set, cql_int32 row) {
  int32_t count = cql_result_set_get_count(result_set);
  cql_contract(row < count);

  cql_result_set_meta *meta = cql_result_set_get_meta(result_set);
  cql_uint16 refs_count = meta->refsCount;
  cql_uint16 refs_offset = meta->refsOffset;
  size_t row_size = meta->rowsize;
  char *data = ((char *)cql_result_set_get_data(result_set)) + row * row_size;

  return cql_hash_buffer(data, row_size, refs_count, refs_offset);
}

static cql_bool cql_buffers_equal(
  const char *_Nonnull data1,
  const char *_Nonnull data2,
  size_t row_size,
  cql_uint16 refs_count,
  cql_uint16 refs_offset)
{
  // We'll do a normal memory comparison on everything up to the first reference type
  // note: the refs are all guaranteed to be at the end AND the padding
  // is guaranteed to be zero-filled.  These are important invariants that
  // let us do a much simpler/faster/smaller comparison.
  size_t size = row_size;
  if (refs_count) {
    size = refs_offset;
  }

  if (memcmp(data1, data2, size)) {
    return false;
  }

  if (refs_count) {
    // first entry is the count, then there are count more entries hence loop <= count
    for (int32_t i = 0; i < refs_count; i++) {
      if (!cql_ref_equal(*(cql_type_ref *)(data1 + refs_offset),
                         *(cql_type_ref *)(data2 + refs_offset))) {
        return false;
      }
      refs_offset += sizeof(cql_type_ref);
    }
  }

  return true;
}

// Check for equality of rows using the metadata to drive the comparison.
// Similar to hashing about we compare the non-references part of the rows
// by checking the leading part and doing a bytewise comparison.  Note that
// any padding is always carefully zeroed out so we can memcmp that as well.
// If that bit matches then we can use the reference equality helper on
// each reference type.  Again we have this general helper so that the
// codegen for result sets can be more economical.  All result sets can use this one
// function.
cql_bool cql_rows_equal(
  cql_result_set_ref _Nonnull rs1,
  cql_int32 row1,
  cql_result_set_ref _Nonnull rs2,
  cql_int32 row2)
{
  int32_t count1 = cql_result_set_get_count(rs1);
  int32_t count2 = cql_result_set_get_count(rs2);
  cql_contract(row1 < count1);
  cql_contract(row2 < count2);

  // get offsets and verify this is the SAME metadata
  cql_result_set_meta *meta1 = cql_result_set_get_meta(rs1);
  cql_result_set_meta *meta2 = cql_result_set_get_meta(rs2);
  cql_uint16 refs_count = meta1->refsCount;
  cql_uint16 refs_offset = meta1->refsOffset;
  cql_contract(meta2->refsCount == refs_count);
  cql_contract(meta2->refsOffset == refs_offset);

  size_t row_size = meta1->rowsize;
  char *data1 = ((char *)cql_result_set_get_data(rs1)) + row1 * row_size;
  char *data2 = ((char *)cql_result_set_get_data(rs2)) + row2 * row_size;

  return cql_buffers_equal(data1, data2, row_size, refs_count, refs_offset);
}

// sizes for the various data types (not null)
static cql_int32 normal_datasizes[] = {
  0,                             // 0: unused
  sizeof(cql_int32),             // 1: CQL_DATA_TYPE_INT32
  sizeof(cql_int64),             // 2: CQL_DATA_TYPE_INT64
  sizeof(double),                // 3: CQL_DATA_TYPE_DOUBLE
  sizeof(cql_bool),              // 4: CQL_DATA_TYPE_BOOL
};

// sizes for the various data types (nullable)
static cql_int32 nullable_datasizes[] = {
  0,                             // 0: unused
  sizeof(cql_nullable_int32),    // 1: CQL_DATA_TYPE_INT32 (nullable)
  sizeof(cql_nullable_int64),    // 2: CQL_DATA_TYPE_INT64 (nullable)
  sizeof(cql_nullable_double),   // 3: CQL_DATA_TYPE_DOUBLE (nullable)
  sizeof(cql_nullable_bool),     // 4: CQL_DATA_TYPE_BOOL (nullable)
};

// This helper is a little trickier than the strict equality.  "Sameness"
// is defined by a set of columns that correspond to the rows identity.
// CQL doesn't know what that means but the columns can be specified and
// presumably it's meaningful.  So for instance the "keys" of a row
// might need to be compared.  Note that the two result sets must have
// exactly the same shape as defined by the metadata in order to be comparable.
// To do the comparison we have to check each identity column.  If it's
// a reference type then we use the reference type comparison helper and
// otherwise we use strict memory comparison.  There's more decoding because
// you can skip columns and column order is not guaranteed to be offset order.
cql_bool cql_rows_same(
  cql_result_set_ref _Nonnull rs1,
  cql_int32 row1,
  cql_result_set_ref _Nonnull rs2,
  cql_int32 row2)
{
  int32_t count1 = cql_result_set_get_count(rs1);
  int32_t count2 = cql_result_set_get_count(rs2);
  cql_contract(row1 < count1);
  cql_contract(row2 < count2);

  cql_result_set_meta *meta1 = cql_result_set_get_meta(rs1);
  cql_result_set_meta *meta2 = cql_result_set_get_meta(rs2);
  cql_contract(memcmp(meta1, meta2, sizeof(cql_result_set_meta)) == 0);

  cql_contract(meta1->identityColumns);
  uint16_t identityColumnCount = meta1->identityColumns[0];
  cql_contract(identityColumnCount > 0);
  uint16_t *identityColumns = &(meta1->identityColumns[1]);
  uint16_t *columnOffsets = &(meta1->columnOffsets[1]);

  size_t row_size = meta1->rowsize;
  char *data1 = ((char *)cql_result_set_get_data(rs1)) + row1 * row_size;
  char *data2 = ((char *)cql_result_set_get_data(rs2)) + row2 * row_size;

  for (uint16_t i = 0; i < identityColumnCount; i++) {
    uint16_t col = identityColumns[i];
    uint16_t offset = columnOffsets[col];
    // note: the refs are all guaranteed to be at the end AND the padding
    // is guaranteed to be zero-filled.  These are important invariants that
    // let us do a much simpler/faster/smaller comparison.
    if (offset < meta1->refsOffset) {
      // note: the column offsets are not in order because all refs are moved to the end
      // so we compute the size using the datatype (there is a small lookup table for our few types)
      uint8_t type  = meta1->dataTypes[col];
      cql_bool notnull = !!(type & CQL_DATA_TYPE_NOT_NULL);
      type &= CQL_DATA_TYPE_CORE;
      size_t size = notnull ? normal_datasizes[type] : nullable_datasizes[type];
      if (memcmp(data1 + offset, data2 + offset, size)) {
        return false;
      }
    } else {
      // this is a ref type
      if (!cql_ref_equal(*(cql_type_ref *)(data1 + offset), *(cql_type_ref *)(data2 + offset))) {
        return false;
      }
    }
  }

  return true;
}

// This helper allows you to copy out some of the rows of a result set to make a new result set.
// The helper uses only metadata to do its job so, as with the others, codegen
// for this is very economical.  The result set includes in it already all the
// metadata necessary to do the column.
//  * allocate data for the row count times rowsize
//  * memcpy the old data into the new
//  * add 1 to the retain count of all the references in the new data
//  * wrap it all in a result set object
//  * profit :D
void cql_rowset_copy(
  cql_result_set_ref _Nonnull result_set,
  cql_result_set_ref _Nonnull *_Nonnull to_result_set,
  int32_t from,
  cql_int32 count)
{
  cql_contract(from >= 0);
  cql_contract(from + count <= cql_result_set_get_count(result_set));

  // get offsets and rowsize metadata
  cql_result_set_meta *meta = cql_result_set_get_meta(result_set);
  cql_uint16 refs_count = meta->refsCount;
  cql_uint16 refs_offset = meta->refsOffset;

  size_t row_size = cql_result_set_get_meta(result_set)->rowsize;

  char *new_data = calloc(count, row_size);
  char *old_data = ((char *)cql_result_set_get_data(result_set))+ row_size * from;
  memcpy(new_data, old_data, count * row_size);

  char *row = new_data;
  for (int32_t i = 0; i < count; i++, row += row_size) {
    cql_retain_offsets(row, refs_count, refs_offset);
  }

  *to_result_set = cql_result_set_create(new_data, count, *meta);
}

// This method is the workhorse of result set reading, the contract is a bit
// unusual again to allow for economy in the generated code.  Most of the error
// checking of result set access actually happens here in a generic fashion.
// The checks needed are as follows:
//  * the row requested must be in range
//  * the column requested must be in range
//  * the data type of the column must be the requested type
//     * but it could be the nullable version of the same type
//  * the exact data type (including nullability) is stored in "type"
//    * so type is an in/out parameter, it begins with the base type like "int32"
//    * its result is the exact type like "int32" or "nullable int32"
//  * the return value is the addresss of the indicated column
//
// If one of the contracts fails it means:
//   * the provided row/column value is bogus, or uninitialized
//   * the result set object is bogus, it's not a result set at all for instance
//   * the result set object has been previously freed
//   * the result set provided is actually the wrong one
//      * maybe there are several in play
//   * the code that is accessing the result set was recompiled but the code
//     that creates the result set was not, now they disagree as to how many
//     columns there are and what type they are.
// You can use the "meta" object below to debug these situations.
//   * does the meta object look reasonable
//     * number of columns is not negative, or huge
//     * data types of each of the columns is one of the legal values
//       * see (e.g.) CQL_DATA_TYPE_INT32 in cqlrt_common.h
//     * rowsize seems reasonabe (e.g. not negative or massive)
//   * if the rowset looks reasonable then see if you're passing the right one in
//   * if the rowset looks unreasonable, maybe it's been freed and you're looking at stale memory
//   * if the rowset pointer looks insane, maybe its value was never initialized or something like that.
//
// If one of the contracts does fail, look a few frames up the stack for the source of the problem.
// This helper code is pretty stupid and it's unlikely there is a problem actually in this code.
char *_Nonnull cql_address_of_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 col,
  cql_int32 *_Nonnull type)
{
  // Check to make sure the requested row is a valid row
  // See above for reasons why this might fail.
  cql_int32 count = cql_result_set_get_count(result_set);
  cql_contract(row < count);

  // Check to make sure the meta data has column data
  // See above for reasons why this might fail.
  cql_result_set_meta *meta = cql_result_set_get_meta(result_set);
  cql_contract(meta->columnOffsets != NULL);

  // Check to make sure the requested column is a valid column
  // See above for reasons why this might fail.
  int32_t columnCount = meta->columnCount;
  cql_contract(col < columnCount);

  // Check to make sure the requested column is of the correct type
  // See above for reasons why this might fail.
  uint8_t data_type = meta->dataTypes[col];
  cql_contract(CQL_CORE_DATA_TYPE_OF(data_type) == *type);
  *type = data_type;

  // We have a valid row and column so it's safe to do the real work
  // Get the column offset, and rowsize and do the math to compute the data pointer.
  cql_uint16 offset = meta->columnOffsets[col + 1];
  size_t row_size = meta->rowsize;
  return ((char *)cql_result_set_get_data(result_set)) + row * row_size + offset;
}

// This is the helper method that reads an int32 out of a rowset at a particular row and column.
// The same helper is used for reading the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
cql_int32 cql_result_set_get_int32_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row, cql_int32 col)
{
  cql_int32 data_type = CQL_DATA_TYPE_INT32;
  char *data = cql_address_of_col(result_set, row, col, &data_type);

  if (data_type & CQL_DATA_TYPE_NOT_NULL) {
    return *(cql_int32 *)data;
  }
  return ((cql_nullable_int32 *)data)->value;
}

// This is the helper method that write an int32 into a rowset at a particular row and column.
// The same helper is used for writing the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
void cql_result_set_set_int32_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 col,
  cql_int32 new_value)
{
  cql_int32 data_type = CQL_DATA_TYPE_INT32;
  char *data = cql_address_of_col(result_set, row, col, &data_type);

  if (data_type & CQL_DATA_TYPE_NOT_NULL) {
    *(cql_int32 *)data = new_value;
  } else {
    ((cql_nullable_int32 *)data)->value = new_value;
    ((cql_nullable_int32 *)data)->is_null = false;
  }
}

// This is the helper method that reads an int64 out of a rowset at a particular row and column.
// The same helper is used for reading the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
cql_int64 cql_result_set_get_int64_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 col)
{
  cql_int32 data_type = CQL_DATA_TYPE_INT64;
  char *data = cql_address_of_col(result_set, row, col, &data_type);

  if (data_type & CQL_DATA_TYPE_NOT_NULL) {
    return *(cql_int64 *)data;
  }
  return ((cql_nullable_int64 *)data)->value;
}

// This is the helper method that write an int64 into a rowset at a particular row and column.
// The same helper is used for writing the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
void cql_result_set_set_int64_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 col,
  cql_int64 new_value)
{
  cql_int32 data_type = CQL_DATA_TYPE_INT64;
  char *data = cql_address_of_col(result_set, row, col, &data_type);

  if (data_type & CQL_DATA_TYPE_NOT_NULL) {
    *(cql_int64 *)data = new_value;
  } else {
    ((cql_nullable_int64 *)data)->value = new_value;
    ((cql_nullable_int64 *)data)->is_null = false;
  }
}

// This is the helper method that reads a double out of a rowset at a particular row and column.
// The same helper is used for reading the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
cql_double cql_result_set_get_double_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 col)
{
  cql_int32 data_type = CQL_DATA_TYPE_DOUBLE;
  char *data = cql_address_of_col(result_set, row, col, &data_type);

  if (data_type & CQL_DATA_TYPE_NOT_NULL) {
    return *(cql_double *)data;
  }
  return ((cql_nullable_double *)data)->value;
}

// This is the helper method that write an double into a rowset at a particular row and column.
// The same helper is used for writing the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
void cql_result_set_set_double_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 col,
  cql_double new_value)
{
  cql_int32 data_type = CQL_DATA_TYPE_DOUBLE;
  char *data = cql_address_of_col(result_set, row, col, &data_type);

  if (data_type & CQL_DATA_TYPE_NOT_NULL) {
    *(cql_double *)data = new_value;
  } else {
    ((cql_nullable_double *)data)->value = new_value;
    ((cql_nullable_double *)data)->is_null = false;
  }
}

// This is the helper method that reads an bool out of a rowset at a particular row and column.
// The same helper is used for reading the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
cql_bool cql_result_set_get_bool_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row, cql_int32 col)
{
  cql_int32 data_type = CQL_DATA_TYPE_BOOL;
  char *data = cql_address_of_col(result_set, row, col, &data_type);

  if (data_type & CQL_DATA_TYPE_NOT_NULL) {
    return *(cql_bool *)data;
  }
  return ((cql_nullable_bool *)data)->value;
}

// This is the helper method that write an bool into a rowset at a particular row and column.
// The same helper is used for writing the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
void cql_result_set_set_bool_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 col,
  cql_bool new_value)
{
  cql_int32 data_type = CQL_DATA_TYPE_BOOL;
  char *data = cql_address_of_col(result_set, row, col, &data_type);

  if (data_type & CQL_DATA_TYPE_NOT_NULL) {
    *(cql_bool *)data = new_value;
  } else {
    ((cql_nullable_bool *)data)->value = new_value;
    ((cql_nullable_bool *)data)->is_null = false;
  }
}

// This is the helper method that reads a string out of a rowset at a particular row and column.
// The same helper is used for reading the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
cql_string_ref _Nullable cql_result_set_get_string_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row, cql_int32 col)
{
  cql_int32 data_type = CQL_DATA_TYPE_STRING;
  char *data = cql_address_of_col(result_set, row, col, &data_type);
  return *(cql_string_ref *)data;
}

// This is the helper method that write an string into a rowset at a particular row and column.
// The same helper is used for writing the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
void cql_result_set_set_string_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 col,
  cql_string_ref _Nullable new_value)
{
  cql_int32 data_type = CQL_DATA_TYPE_STRING;
  char *data = cql_address_of_col(result_set, row, col, &data_type);
  cql_set_string_ref((cql_string_ref *)data, new_value);
}

// This is the helper method that reads a object out of a rowset at a particular row and column.
// The same helper is used for reading the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
cql_object_ref _Nullable cql_result_set_get_object_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 col)
{
  cql_int32 data_type = CQL_DATA_TYPE_OBJECT;
  char *data = cql_address_of_col(result_set, row, col, &data_type);
  return *(cql_object_ref *)data;
}

// This is the helper method that write an object into a rowset at a particular row and column.
// The same helper is used for writing the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
void cql_result_set_set_object_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 col,
  cql_object_ref _Nullable new_value)
{
  cql_int32 data_type = CQL_DATA_TYPE_OBJECT;
  char *data = cql_address_of_col(result_set, row, col, &data_type);
  cql_set_object_ref((cql_object_ref *)data, new_value);
}

// This is the helper method that reads a blob out of a rowset at a particular row and column.
// The same helper is used for reading the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
cql_blob_ref _Nullable cql_result_set_get_blob_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 col)
{
  cql_int32 data_type = CQL_DATA_TYPE_BLOB;
  char *data = cql_address_of_col(result_set, row, col, &data_type);
  return *(cql_blob_ref *)data;
}

// This is the helper method that write an blob into a rowset at a particular row and column.
// The same helper is used for writing the value from a nullable or not nullable value, so the address helper
// has to report which kind of datum it is.  All the error checking is in cql_address_of_col.
void cql_result_set_set_blob_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row,
  cql_int32 col,
  cql_blob_ref _Nullable new_value)
{
  cql_int32 data_type = CQL_DATA_TYPE_BLOB;
  char *data = cql_address_of_col(result_set, row, col, &data_type);
  cql_set_blob_ref((cql_blob_ref *)data, new_value);
}

// This is the helper method that determines if a nullable column column is null or not.
// If the data type of the column is string or blob then we look for a null value for the pointer in question
// If the data type is not nullable, we return false.
// If the data type is nullable then we read the is_null value out of the row
cql_bool cql_result_set_get_is_null_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row, cql_int32 col)
{
  // Check to make sure the requested row is a valid row
  // See cql_address_of_col for reasons why this might fail.
  cql_int32 count = cql_result_set_get_count(result_set);
  cql_contract(row < count);

  // Check to make sure the meta data has column data
  // See cql_address_of_col for reasons why this might fail.
  cql_result_set_meta *meta = cql_result_set_get_meta(result_set);
  cql_contract(meta->columnOffsets != NULL);

  // Check to make sure the requested column is a valid column
  // See cql_address_of_col for reasons why this might fail.
  int32_t columnCount = meta->columnCount;
  cql_contract(col < columnCount);

  uint8_t data_type = meta->dataTypes[col];

  cql_uint16 offset = meta->columnOffsets[col + 1];
  size_t row_size = meta->rowsize;
  char *data =((char *)cql_result_set_get_data(result_set)) + row * row_size + offset;

  int32_t core_data_type = CQL_CORE_DATA_TYPE_OF(data_type);

  if (core_data_type == CQL_DATA_TYPE_BLOB
    || core_data_type == CQL_DATA_TYPE_STRING
    || core_data_type == CQL_DATA_TYPE_OBJECT) {
     return !*(void **)data;
  }

  if (data_type & CQL_DATA_TYPE_NOT_NULL) {
     return false;
  }

  cql_bool is_null = 1;

  switch (core_data_type) {
    case CQL_DATA_TYPE_BOOL:
     is_null = ((cql_nullable_bool *)data)->is_null;
     break;

    case CQL_DATA_TYPE_INT32:
     is_null = ((cql_nullable_int32 *)data)->is_null;
     break;

    case CQL_DATA_TYPE_INT64:
     is_null = ((cql_nullable_int64 *)data)->is_null;
     break;

    default:
     // nothing else left
     cql_contract(core_data_type == CQL_DATA_TYPE_DOUBLE);
     is_null = ((cql_nullable_double *)data)->is_null;
     break;
  }

  return is_null;
}

// This is the helper method that sets a nullable column to null
void cql_result_set_set_to_null_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 row, cql_int32 col)
{
  // Check to make sure the requested row is a valid row
  // See cql_address_of_col for reasons why this might fail.
  cql_int32 count = cql_result_set_get_count(result_set);
  cql_contract(row < count);

  // Check to make sure the meta data has column data
  // See cql_address_of_col for reasons why this might fail.
  cql_result_set_meta *meta = cql_result_set_get_meta(result_set);
  cql_contract(meta->columnOffsets != NULL);

  // Check to make sure the requested column is a valid column
  // See cql_address_of_col for reasons why this might fail.
  int32_t columnCount = meta->columnCount;
  cql_contract(col < columnCount);

  uint8_t data_type = meta->dataTypes[col];

  cql_uint16 offset = meta->columnOffsets[col + 1];
  size_t row_size = meta->rowsize;
  char *data =((char *)cql_result_set_get_data(result_set)) + row * row_size + offset;

  int32_t core_data_type = CQL_CORE_DATA_TYPE_OF(data_type);

  // if this fails you are attempting to set a not null column to null
  cql_contract(!(data_type & CQL_DATA_TYPE_NOT_NULL));

  // if this fails it means you're using the null set helper on an reference type
  // you can just use the normal setter on those types because they are references
  // and so NULL is valid.  You only use this method for setting primitive types to null.
  cql_contract(core_data_type != CQL_DATA_TYPE_BLOB);
  cql_contract(core_data_type != CQL_DATA_TYPE_STRING);
  cql_contract(core_data_type != CQL_DATA_TYPE_OBJECT);

  switch (core_data_type) {
    case CQL_DATA_TYPE_BOOL:
      cql_set_null(*(cql_nullable_bool *)data);
      break;

    case CQL_DATA_TYPE_INT32:
      cql_set_null(*(cql_nullable_int32 *)data);
      break;

    case CQL_DATA_TYPE_INT64:
      cql_set_null(*(cql_nullable_int64 *)data);
      break;

    default:
     // nothing else left but double
     cql_contract(core_data_type == CQL_DATA_TYPE_DOUBLE);
     cql_set_null(*(cql_nullable_double *)data);
     break;
  }
}

// This is the helper method that determines if a column is encoded
// return TRUE if the data type value has the flag CQL_DATA_TYPE_ENCODED
cql_bool cql_result_set_get_is_encoded_col(
  cql_result_set_ref _Nonnull result_set,
  cql_int32 col)
{
  cql_result_set_meta *meta = cql_result_set_get_meta(result_set);

  // Check to make sure the requested column is a valid column
  // See cql_address_of_col for reasons why this might fail.
  int32_t columnCount = meta->columnCount;
  cql_contract(col < columnCount);

  return !!(meta->dataTypes[col] & CQL_DATA_TYPE_ENCODED);
}

// Tables contains a list of tables we need to drop.  The format is
// table1\0table2\0table3\0\0
// We try to drop all those tables.
static void cql_autodrop_tables(sqlite3 *_Nullable db, const char *_Nullable tables) {
  if (!tables) {
    return;
  }

  // semantic analysis prevents any autodrop tables in cases where there is no db pointer
  cql_contract(db);

  const char *drop_table = "DROP TABLE IF EXISTS ";
  const char *p = tables;
  int32_t max_len = 0;
  int32_t drop_len = (int32_t)strlen(drop_table);

  // find the longest table name so we can make a suitable buffer
  for (;;) {
    // stop when we find the zero length table name
    int32_t len = (int32_t)strlen(p);
    if (!len) {
      break;
    }

    if (len > max_len) {
      max_len = len;
    }

    p += len + 1;
  }

  // we need enough room for the drop command plus the longest table name
  // plus the ";" and the null.
  STACK_BYTES_ALLOC(sql, drop_len + max_len + 2);

  // this part will be constant for all the iterations
  strcpy(sql, drop_table);

  p = tables;
  for (;;) {
    // stop when we find the zero length table name
    int32_t len = (int32_t)strlen(p);
    if (!len) {
      break;
    }

    // form the drop command from the fragments
    strcpy(sql + drop_len, p);
    strcpy(sql + drop_len + len, ";");

    // Try to drop the table, if it fails we disregard the failure code
    // there's nothing we could do to recover anyway.
    cql_exec(db, sql);

    p += len + 1;
  }
}

void cql_initialize_meta(cql_result_set_meta *_Nonnull meta, cql_fetch_info *_Nonnull info) {
  memset(meta, 0, sizeof(*meta));
  meta->teardown = cql_result_set_teardown;
  meta->rowsize = info->rowsize;
  meta->rowHash = cql_row_hash;
  meta->rowsEqual = cql_rows_equal;
  meta->rowsSame = cql_rows_same;
  meta->refsCount = info->refs_count;
  meta->refsOffset = info->refs_offset;
  meta->columnOffsets = info->col_offsets;
  meta->columnCount = info->col_offsets[0];
  meta->identityColumns = info->identity_columns;
  meta->dataTypes = info->data_types;
  meta->encodeContextIndex = info->encode_context_index;
  meta->copy = cql_rowset_copy;
  #ifndef CQL_NO_GETTERS
      meta->getBoolean = cql_result_set_get_bool_col;
      meta->getDouble = cql_result_set_get_double_col;
      meta->getInt32 = cql_result_set_get_int32_col;
      meta->getInt64 = cql_result_set_get_int64_col;
      meta->getString = cql_result_set_get_string_col;
      meta->getObject = cql_result_set_get_object_col;
      meta->getBlob = cql_result_set_get_blob_col;
      meta->getIsNull = cql_result_set_get_is_null_col;
      meta->getIsEncoded = cql_result_set_get_is_encoded_col;

      meta->setBoolean = cql_result_set_set_bool_col;
      meta->setDouble = cql_result_set_set_double_col;
      meta->setInt32 = cql_result_set_set_int32_col;
      meta->setInt64 = cql_result_set_set_int64_col;
      meta->setString = cql_result_set_set_string_col;
      meta->setObject = cql_result_set_set_object_col;
      meta->setBlob = cql_result_set_set_blob_col;
      meta->setToNull = cql_result_set_set_to_null_col;
  #endif
}

// true if any of the columns of this result set are to be encoded
// all we have to do is check the encoded bit on the data types
static cql_bool cql_are_any_encoded(cql_fetch_info *_Nonnull info) {
  uint8_t *_Nonnull data_types = info->data_types;
  uint16_t *_Nonnull col_offsets = info->col_offsets;
  uint32_t count = col_offsets[0];

  for (cql_int32 column = 0; column < count; column++) {
    uint8_t type = data_types[column];
    if (type & CQL_DATA_TYPE_ENCODED) {
      return true;
    }
  }
  return false;
}

// By the time we get here, a CQL stored proc has completed execution and there is
// now a statement (or an error result).  This function iterates the rows that
// come out of the statement using the fetch info to describe the shape of the
// expected results.  All of this code is shared so that the cost of any given
// stored procedure is minimized.  Even the error handling is consolidated.
cql_code cql_fetch_all_results(
  cql_fetch_info *_Nonnull info,
  cql_result_set_ref _Nullable *_Nonnull result_set)
{
  *result_set = NULL;
  int32_t count = 0;
  cql_bytebuf b;
  cql_bytebuf_open(&b);
  sqlite3_stmt *stmt = info->stmt;
  int32_t rowsize = info->rowsize;
  char *row;
  cql_code rc = info->rc;

  if (rc != SQLITE_OK) goto cql_error;

  if (cql_are_any_encoded(info)) {
    info->encoder = cql_copy_encoder(info->db);
  }

  for (;;) {
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) goto cql_error;
    count++;
    row = cql_bytebuf_alloc(&b, rowsize);
    memset(row, 0, rowsize);

    cql_multifetch_meta((char *)row, info);
  }

  // If all is well, we close the statement and we're done with OK result.
  // If anything went wrong we free all the memory and we're outta here.

  cql_finalize_stmt(&stmt);
  cql_result_set_meta meta;
  cql_initialize_meta(&meta, info);
  cql_object_release(info->encoder); // nullsafe
  info->encoder = NULL;

  *result_set = cql_result_set_create(b.ptr, count, meta);
  cql_autodrop_tables(info->db, info->autodrop_tables);
  cql_profile_stop(info->crc, info->perf_index);
  return SQLITE_OK;

cql_error:
  // If we have allocated any rows, and they need cleanup, clean them up now
  if (info->refs_count) {
    row = b.ptr;
    for (cql_int32 i = 0; i < count ; i++, row += rowsize) {
      cql_release_offsets(row, info->refs_count, info->refs_offset);
    }
  }
  cql_bytebuf_close(&b);
  cql_finalize_stmt(&stmt);
  cql_log_database_error(info->db, "cql", "database error");
  cql_autodrop_tables(info->db, info->autodrop_tables);
  cql_object_release(info->encoder); // nullsafe
  info->encoder = NULL;
  cql_profile_stop(info->crc, info->perf_index);
  return rc;
}

// As soon as a new result_set is created. The result_set's field needs
// to be encoded if they're sensitive and has the bit CQL_DATA_TYPE_ENCODED.
// We only encode result_set's field when creating the result_set for:
//  - [OUT C] statement: @see cql_one_row_result(...)
//  - [OUT UNION C] statement: @see cql_results_from_data(...)
// We also decode when reading fields from a result set (see cql_copyoutrow(...))
// If applicable this helper encode the result_set rows of the newly created result_set.
static void cql_encode_new_result_set_data(
  cql_fetch_info *_Nonnull info,
  char *_Nullable data,
  cql_int32 rows)
{
  sqlite3 *db = info->db;
  if (!db) {
    // DB pointer is only set if the result_set is a came from the database.
    // We also only encode/decode if the result_set came from the database.
    // Non database result sets (e.g. out union) are never encoded/decoded
    // therefore we should end here.
    return;
  }

  int32_t rowsize = info->rowsize;
  uint8_t *_Nonnull data_types = info->data_types;
  uint16_t *_Nonnull col_offsets = info->col_offsets;

  uint32_t count = col_offsets[0];
  col_offsets++;

  cql_bool got_encoder = false;
  cql_object_ref encoder = NULL;

  cql_int32 encode_context_type = -1;
  if (info->encode_context_index >= 0) {
    encode_context_type = data_types[info->encode_context_index];
  }
  char *encode_context_field = NULL;
  char *row = data;
  for (cql_int32 i = 0; i < rows ; i++, row += rowsize) {
    if (info->encode_context_index >= 0) {
      encode_context_field = row + col_offsets[info->encode_context_index];
    }
    for (cql_int32 column = 0; column < count; column++) {
      uint8_t type = data_types[column];
      char *field = row + col_offsets[column];

      if (!got_encoder && type & CQL_DATA_TYPE_ENCODED) {
        encoder = cql_copy_encoder(db);
        got_encoder = true;
      }

      switch (type) {
        case CQL_DATA_TYPE_INT32 | CQL_DATA_TYPE_ENCODED | CQL_DATA_TYPE_NOT_NULL: {
          cql_int32 *int32_data = (cql_int32 *)field;
          *int32_data = cql_encode_int32(encoder, *int32_data, encode_context_type, encode_context_field);
          break;
        }
        case CQL_DATA_TYPE_INT64 | CQL_DATA_TYPE_ENCODED | CQL_DATA_TYPE_NOT_NULL: {
          cql_int64 *int64_data = (cql_int64 *)field;
          *int64_data = cql_encode_int64(encoder, *int64_data, encode_context_type, encode_context_field);
          break;
        }
        case CQL_DATA_TYPE_DOUBLE | CQL_DATA_TYPE_ENCODED | CQL_DATA_TYPE_NOT_NULL: {
          cql_double *double_data = (cql_double *)field;
          *double_data = cql_encode_double(encoder, *double_data, encode_context_type, encode_context_field);
          break;
        }
        case CQL_DATA_TYPE_BOOL | CQL_DATA_TYPE_ENCODED | CQL_DATA_TYPE_NOT_NULL: {
          cql_bool *bool_data = (cql_bool *)field;
          *bool_data = cql_encode_bool(encoder, *bool_data, encode_context_type, encode_context_field);
          break;
        }
        case CQL_DATA_TYPE_STRING | CQL_DATA_TYPE_ENCODED | CQL_DATA_TYPE_NOT_NULL: {
          cql_string_ref *str_ref = (cql_string_ref *)field;
          cql_string_ref new_str_ref = cql_encode_string_ref_new(encoder, *str_ref, encode_context_type, encode_context_field);
          cql_set_string_ref(str_ref, new_str_ref);
          cql_string_release(new_str_ref);
          break;
        }
        case CQL_DATA_TYPE_BLOB | CQL_DATA_TYPE_ENCODED | CQL_DATA_TYPE_NOT_NULL: {
          cql_blob_ref *blob_ref = (cql_blob_ref *)field;
          cql_blob_ref new_blob_ref = cql_encode_blob_ref_new(encoder, *blob_ref, encode_context_type, encode_context_field);
          cql_set_blob_ref(blob_ref, new_blob_ref);
          cql_blob_release(new_blob_ref);
          break;
        }
        case CQL_DATA_TYPE_INT32 | CQL_DATA_TYPE_ENCODED: {
          cql_nullable_int32 *_Nonnull int32p = (cql_nullable_int32 *_Nonnull)field;
          if (!int32p->is_null) {
            int32p->value = cql_encode_int32(encoder, int32p->value, encode_context_type, encode_context_field);
          }
          break;
        }
        case CQL_DATA_TYPE_INT64 | CQL_DATA_TYPE_ENCODED: {
          cql_nullable_int64 *_Nonnull int64p = (cql_nullable_int64 *_Nonnull)field;
          if (!int64p->is_null) {
            int64p->value = cql_encode_int64(encoder, int64p->value, encode_context_type, encode_context_field);
          }
          break;
        }
        case CQL_DATA_TYPE_DOUBLE | CQL_DATA_TYPE_ENCODED: {
          cql_nullable_double *_Nonnull doublep = (cql_nullable_double *_Nonnull)field;
          if (!doublep->is_null) {
            doublep->value = cql_encode_double(encoder, doublep->value, encode_context_type, encode_context_field);
          }
          break;
        }
        case CQL_DATA_TYPE_BOOL | CQL_DATA_TYPE_ENCODED: {
          cql_nullable_bool *_Nonnull boolp = (cql_nullable_bool *_Nonnull)field;
          if (!boolp->is_null) {
            boolp->value = cql_encode_bool(encoder, boolp->value, encode_context_type, encode_context_field);
          }
          break;
        }
        case CQL_DATA_TYPE_STRING | CQL_DATA_TYPE_ENCODED: {
          cql_string_ref *str_ref = (cql_string_ref *)field;
          if (*str_ref) {
            cql_string_ref new_str_ref = cql_encode_string_ref_new(encoder, *str_ref, encode_context_type, encode_context_field);
            cql_set_string_ref(str_ref, new_str_ref);
            cql_string_release(new_str_ref);
          }
          break;
        }
        case CQL_DATA_TYPE_BLOB | CQL_DATA_TYPE_ENCODED: {
          cql_blob_ref *blob_ref = (cql_blob_ref *)field;
          if (*blob_ref) {
            cql_blob_ref new_blob_ref = cql_encode_blob_ref_new(encoder, *blob_ref, encode_context_type, encode_context_field);
            cql_set_blob_ref(blob_ref, new_blob_ref);
            cql_blob_release(new_blob_ref);
          }
          break;
        }
      }
    }
  }
  cql_object_release(encoder);
}

// In this result set creator, the rows are sitting pretty in a buffer we've already
// constructed. The return code tells us if we're exiting clean or not.  If we're
// not clean then the buffer should be disposed, there will be no result set returned.
void cql_results_from_data(
  cql_code rc,
  cql_bytebuf *_Nonnull buffer,
  cql_fetch_info *_Nonnull info,
  cql_result_set_ref _Nullable *_Nonnull result_set)
{
  *result_set = NULL;
  int32_t rowsize = info->rowsize;
  int32_t count = buffer->used / rowsize;

  if (rc == SQLITE_OK) {
    cql_result_set_meta meta;
    cql_initialize_meta(&meta, info);
    // We need to encode the column value of the new result_set's data. We're only
    // encoding result_set because it's the final output of a stored proc.
    cql_encode_new_result_set_data(info, buffer->ptr, count);
    *result_set = cql_result_set_create(buffer->ptr, count, meta);
  }
  else {
    if (info->refs_count) {
      char *row = buffer->ptr;
      for (cql_int32 i = 0; i < count ; i++, row += rowsize) {
        cql_release_offsets(row, info->refs_count, info->refs_offset);
      }
    }
    cql_bytebuf_close(buffer);
  }

  cql_autodrop_tables(info->db, info->autodrop_tables);
  cql_profile_stop(info->crc, info->perf_index);
}

// Just like cql_fetch_all_results but for the "one row result" case
// In that case the data has already been fetched.  Its shape is
// described just like the above.  All we need to do is wrap the row
// in a result set and we're done.  As above the error cases are also
// handled here.
cql_code cql_one_row_result(
  cql_fetch_info *_Nonnull info,
  char *_Nullable data,
  int32_t count,
  cql_result_set_ref _Nullable *_Nonnull result_set)
{
  cql_code rc = info->rc;
  *result_set = NULL;
  if (rc != SQLITE_OK) goto cql_error;

  cql_result_set_meta meta;
  cql_initialize_meta(&meta, info);
  // We need to encode the column value of the new result_set's data. We're only
  // encoding result_set because it's the final output of a stored proc.
  cql_encode_new_result_set_data(info, data, count);
  *result_set = cql_result_set_create(data, count, meta);
  cql_autodrop_tables(info->db, info->autodrop_tables);
  cql_profile_stop(info->crc, info->perf_index);
  return SQLITE_OK;

cql_error:
  cql_release_offsets(data, info->refs_count, info->refs_offset);
  free(data);
  cql_log_database_error(info->db, "cql", "database error");
  cql_autodrop_tables(info->db, info->autodrop_tables);
  cql_profile_stop(info->crc, info->perf_index);
  return rc;
}

// these are some structures we need so that we can make an empty result set
// it has a canonical shape (1 column) but there are no rows
// so no column getter will ever succeed not matter the shape that was
// expected.

typedef struct cql_no_rows_row {
  cql_int32 x;
} cql_no_rows_row;

static int32_t cql_no_rows_row_perf_index;

uint8_t cql_no_rows_row_data_types[] = {
  CQL_DATA_TYPE_INT32 | CQL_DATA_TYPE_NOT_NULL, // x
};

static cql_uint16 cql_no_rows_row_col_offsets[] = { 1,
  cql_offsetof(cql_no_rows_row, x)
};

cql_fetch_info cql_no_rows_row_info = {
  .rc = SQLITE_OK,
  .data_types = cql_no_rows_row_data_types,
  .col_offsets = cql_no_rows_row_col_offsets,
  .rowsize = sizeof(cql_no_rows_row),
  .crc = 0,
  .perf_index = &cql_no_rows_row_perf_index,
};

// The most trivial empty result set that still looks like a result set
cql_result_set_ref _Nonnull cql_no_rows_result_set() {
  cql_result_set_meta meta;
  cql_initialize_meta(&meta, &cql_no_rows_row_info);
  return cql_result_set_create(malloc(1), 0, meta);
}

// This statement for sure has no rows in it
cql_code cql_no_rows_stmt(sqlite3 *_Nonnull db, sqlite3_stmt *_Nullable *_Nonnull pstmt) {
  cql_finalize_stmt(pstmt);
  return cql_sqlite3_prepare_v2(db, "select 0 where 0", -1, pstmt, NULL);
}

// basic closed hash table, small initial size with doubling
#define HASHTAB_INIT_SIZE 4
#define HASHTAB_LOAD_FACTOR .75

// helper to set the payload array, used at init time and during rehash
static void cql_hashtab_set_payload(cql_hashtab *_Nonnull ht) {
  ht->payload = (cql_hashtab_entry *)calloc(ht->capacity, sizeof(cql_hashtab_entry));
}


// fwd ref needed for rehash
static cql_bool cql_hashtab_add(cql_hashtab *_Nonnull ht, cql_int64 key_new, cql_int64 val_new);

// Rehash to a bigger size, all the items are re-inserted.
// Note we have to release the old values because the new values
// are retained upon insertion.  This keeps the reference counting correct.
static void cql_hashtab_rehash(cql_hashtab *_Nonnull ht) {
  uint32_t old_capacity = ht->capacity;
  cql_hashtab_entry *old_payload = ht->payload;

  ht->count = 0;
  ht->capacity *= 2;
  cql_hashtab_set_payload(ht);

  for (uint32_t i = 0; i < old_capacity; i++) {
    cql_int64 key = old_payload[i].key;
    cql_int64 val = old_payload[i].val;
    if (key) {
      cql_hashtab_add(ht, key, val);
      ht->release_key(ht->context, key);
      ht->release_val(ht->context, val);
    }
  }

  free(old_payload);
}

// Making a new hash table, initial size
static cql_hashtab *_Nonnull cql_hashtab_new(
  uint64_t (*_Nonnull hash_key)(void *_Nullable context, cql_int64 key),
  bool (*_Nonnull compare_keys)(void *_Nullable context, cql_int64 key1, cql_int64 key2),
  void (*_Nonnull retain_key)(void *_Nullable context, cql_int64 key),
  void (*_Nonnull retain_val)(void *_Nullable context, cql_int64 val),
  void (*_Nonnull release_key)(void *_Nullable context, cql_int64 key),
  void (*_Nonnull release_val)(void *_Nullable context, cql_int64 val),
  void *_Nullable context
) {
  cql_hashtab *ht = malloc(sizeof(cql_hashtab));
  ht->hash_key = hash_key;
  ht->compare_keys = compare_keys;
  ht->retain_key = retain_key;
  ht->retain_val = retain_val;
  ht->release_key = release_key;
  ht->release_val = release_val;
  ht->count = 0;
  ht->capacity = HASHTAB_INIT_SIZE;
  ht->context = context;
  cql_hashtab_set_payload(ht);
  return ht;
}

// release the memory for the hash table including
// releasing all the strings stored as keys.
static void cql_hashtab_delete(cql_hashtab *_Nonnull ht) {
  for (int32_t i = 0; i < ht->capacity; i++) {
    cql_int64 key = ht->payload[i].key;
    cql_int64 val = ht->payload[i].val;
    if (key) {
      ht->release_key(ht->context, key);
    }
    if (val) {
      ht->release_val(ht->context, val);
    }
  }

  free(ht->payload);
  free(ht);
}

// Add a new key to the hash table
// * if the key is addred return true
// * if the key exists return false and do nothing
static cql_bool cql_hashtab_add(
  cql_hashtab *_Nonnull ht,
  cql_int64 key_new,
  cql_int64 val_new)
{
  uint32_t hash = (uint32_t)ht->hash_key(ht->context, key_new);
  uint32_t offset = hash % ht->capacity;
  cql_hashtab_entry *payload = ht->payload;

  for (;;) {
    cql_int64 key = payload[offset].key;
    if (!key) {
      ht->retain_key(ht->context, key_new);
      ht->retain_val(ht->context, val_new);

      payload[offset].key = key_new;
      payload[offset].val = val_new;

      ht->count++;
      if (ht->count > ht->capacity * HASHTAB_LOAD_FACTOR) {
        cql_hashtab_rehash(ht);
      }

      return true;
    }

    if (ht->compare_keys(ht->context, key, key_new)) {
      return false;
    }

    offset++;
    if (offset >= ht->capacity) {
      offset = 0;
    }
  }
}

// returns the payload item for the indicated key (allowing mutation)
// if the key is not found returns null
static cql_hashtab_entry *_Nullable cql_hashtab_find(
  cql_hashtab *_Nonnull ht,
  cql_int64 key_needed)
{
  uint32_t hash = (uint32_t)ht->hash_key(ht->context, key_needed);
  uint32_t offset = hash % ht->capacity;
  cql_hashtab_entry *payload = ht->payload;

  for (;;) {
    cql_int64 key = ht->payload[offset].key;
    if (!key) {
      return NULL;
    }

    if (ht->compare_keys(ht->context, key, key_needed)) {
      return &payload[offset];
    }

    offset++;
    if (offset >= ht->capacity) {
      offset = 0;
    }
  }
}

// These are CQL friendly versions of the hashtable for a string to int map, these signatures are directly callable from CQL

static void cql_no_op_retain_release(void *_Nullable context, cql_int64 data) {
}

static void cql_key_retain_str(void *_Nullable context, cql_int64 key) {
  if (key) {
    cql_retain((cql_type_ref)(key));
  }
}

static void cql_key_release_str(void *_Nullable context, cql_int64 key) {
  if (key) {
    cql_release((cql_type_ref)(key));
  }
}

static uint64_t cql_key_str_hash(void *_Nullable context, cql_int64 key) {
  return cql_string_hash((cql_string_ref)key);
}

static bool cql_key_str_eq(void *_Nullable context, cql_int64 key1, cql_int64 key2) {
  return cql_string_equal((cql_string_ref)key1, (cql_string_ref)key2);
}

// Defer finalization to the hash table which has all it needs to do the job
static void cql_facets_finalize(void *_Nonnull data) {
  cql_hashtab *_Nonnull self = data;
  cql_hashtab_delete(self);
}

// create the facets storage using the hashtable
cql_object_ref _Nonnull cql_facets_create(void) {

  cql_hashtab * self = cql_hashtab_new(
    cql_key_str_hash,
    cql_key_str_eq,
    cql_key_retain_str,
    cql_no_op_retain_release,
    cql_key_release_str,
    cql_no_op_retain_release,
    NULL
  );

  return _cql_generic_object_create(self, cql_facets_finalize);
}

// add a facet value to the hash table
cql_bool cql_facet_add(cql_object_ref _Nullable facets, cql_string_ref _Nonnull name, cql_int64 crc) {
  cql_bool result = false;
  if (facets) {
    cql_hashtab *_Nonnull self = _cql_generic_object_get_data(facets);
    result = cql_hashtab_add(self, (cql_int64)name, crc);
  }
  return result;
}

// Search for the facet value in the hash table, if not found return -1
cql_int64 cql_facet_find(cql_object_ref _Nullable facets, cql_string_ref _Nonnull name) {
  cql_int64 result = -1;
  if (facets) {
    cql_hashtab *_Nonnull self = _cql_generic_object_get_data(facets);
    cql_hashtab_entry *payload = cql_hashtab_find(self, (cql_int64)name);
    if (payload) {
      result = payload->val;
    }
  }
  return result;
}

// Search for the facet value in the hash table, replace it if it exists
// add it if it doesn't
cql_bool cql_facet_upsert(cql_object_ref _Nullable facets, cql_string_ref _Nonnull name, cql_int64 crc) {
  cql_bool result = false;
  if (facets) {
    cql_hashtab *_Nonnull self = _cql_generic_object_get_data(facets);
    cql_hashtab_entry *payload = cql_hashtab_find(self, (cql_int64)name);
    if (!payload) {
      // this will return true because we just checked and it's not there
      result = cql_hashtab_add(self, (cql_int64)name, crc);
    }
    else {
      // did not add path
      payload->val = crc;
    }
  }

  return result;
}

#define cql_append_value(b, var) cql_bytebuf_append(&b, &var, sizeof(var))

#define cql_append_nullable_value(b, var) \
  if (!var.is_null) { \
    cql_setbit(bits, nullable_index); \
    cql_append_value(b, var.value); \
  }

static void cql_setbit(uint8_t *_Nonnull bytes, uint16_t index) {
  bytes[index / 8] |= (1 << (index % 8));
}

static cql_bool cql_getbit(const uint8_t *_Nonnull bytes, uint16_t index) {
  return !!(bytes[index / 8] & (1 << (index % 8)));
}

typedef struct cql_input_buf {
  const unsigned char *_Nonnull data;
  uint32_t remaining;
} cql_input_buf;

static bool cql_input_read(cql_input_buf *_Nonnull buf, void *_Nonnull dest, uint32_t bytes) {
  if (bytes > buf->remaining) {
    return false;
  }

  memcpy(dest, buf->data, bytes);
  buf->remaining -= bytes;
  buf->data += bytes;

  return true;
}

static bool cql_input_inline_str(
  cql_input_buf *_Nonnull buf,
  const char *_Nonnull *_Nonnull dest)
{
  unsigned char *nullchar = memchr(buf->data, 0, buf->remaining);
  if (nullchar) {
    uint32_t bytes = (uint32_t)(nullchar - buf->data) + 1;
    *dest = (const char *)buf->data;
    buf->remaining -= bytes;
    buf->data += bytes;
    return true;
  }

  return false;
}

static bool cql_input_inline_bytes(
  cql_input_buf *_Nonnull buf,
  const uint8_t *_Nonnull *_Nonnull dest,
  uint32_t bytes)
{
  if (bytes <= buf->remaining) {
    *dest = buf->data;
    buf->remaining -= bytes;
    buf->data += bytes;
    return true;
  }

  return false;
}

static uint32_t cql_zigzag_encode_32 (cql_int32 i) {
  return (i >> 31) ^ (i << 1);
}

static cql_int32 cql_zigzag_decode_32 (uint32_t i) {
  return (i >> 1) ^ -(i & 1);
}

static uint64_t cql_zigzag_encode_64 (cql_int64 i) {
  return (i >> 63) ^ (i << 1);
}

static cql_int64 cql_zigzag_decode_64 (uint64_t i) {
  return (i >> 1) ^ -(i & 1);
}

// variable length encoding using zigzag and 7 bits with extension
// note that this also takes care of any endian issues
static bool cql_read_varint_32(cql_input_buf *_Nonnull buf, cql_int32 *_Nonnull out) {
  uint32_t result = 0;
  uint8_t byte;
  uint8_t i = 0;
  uint8_t offset = 0;
  while (i < 5) {
    if (!cql_input_read(buf, &byte, 1)) {
      return false;
    }
    result |= ((uint32_t)(byte & 0x7f)) << offset;
    if (!(byte & 0x80)) {
      *out = cql_zigzag_decode_32(result);
      return true;
    }
    offset += 7;
    i++;
  }

  // badly formed buffer, 5 bytes is the most we need for a 32 bit varint
  return false;
}

// variable length encoding using zigzag and 7 bits with extension
// note that this also takes care of any endian issues
static bool cql_read_varint_64(cql_input_buf *_Nonnull buf, cql_int64 *_Nonnull out) {
  uint64_t result = 0;
  uint8_t byte;
  uint8_t i = 0;
  uint8_t offset = 0;
  while (i < 10) {
    if (!cql_input_read(buf, &byte, 1)) {
      return false;
    }
    result |= ((uint64_t)(byte & 0x7f)) << offset;
    if (!(byte & 0x80)) {
      *out = cql_zigzag_decode_64(result);
      return true;
    }
    offset += 7;
    i++;
  }

  // badly formed buffer, 10 bytes is the most we need for a 64 bit varint
  return false;
}

// variable length encoding using zigzag and 7 bits with extension
// note that this also takes care of any endian issues
static void cql_write_varint_32(cql_bytebuf *_Nonnull buf, int32_t si) {
  uint32_t i = cql_zigzag_encode_32(si);
  do {
    uint8_t byte = i & 0x7f;
    i >>= 7;
    if (i) {
      byte |= 0x80;
    }
    cql_append_value(*buf, byte);
  } while (i);
}

// variable length encoding using zigzag and 7 bits with extension
// note that this also takes care of any endian issues
static void cql_write_varint_64(cql_bytebuf *_Nonnull buf, int64_t si) {
  uint64_t i = cql_zigzag_encode_64(si);
  do {
    uint8_t byte = i & 0x7f;
    i >>= 7;
    if (i) {
      byte |= 0x80;
    }
    cql_append_value(*buf, byte);
  } while (i);
}

// This standard helper walks any cursor and creates a versionable encoding of it
// in a blob.  The dynamic cursor structure has all the necessary metadata
// about the cursor.  By the time this is called many checks have been made
// about the suitability of this cursor for serialization (e.g. no OBJECT fields).
// As a consequence we get a nice simple strategy that is flexible.
cql_code cql_serialize_to_blob(cql_blob_ref _Nullable *_Nonnull blob, cql_dynamic_cursor *_Nonnull dyn_cursor)
{
  if (!*dyn_cursor->cursor_has_row) {
    return SQLITE_ERROR;
  }

  uint16_t *offsets = dyn_cursor->cursor_col_offsets;
  uint8_t *types = dyn_cursor->cursor_data_types;
  uint16_t count = offsets[0];  // the first index is the count of fields
  uint8_t *cursor = dyn_cursor->cursor_data;  // we will be using char offsets

  cql_bytebuf b;
  cql_bytebuf_open(&b);

  uint8_t code = 0;
  uint16_t nullable_count = 0;
  uint16_t bool_count = 0;

  for (uint16_t i = 0; i < count; i++) {
    uint8_t type = types[i];
    cql_bool nullable = !(type & CQL_DATA_TYPE_NOT_NULL);
    int8_t core_data_type = CQL_CORE_DATA_TYPE_OF(type);

    code = 0;
    if (nullable) {
      nullable_count++;
      code = 'a' - 'A';  // lower case for nullable
    }

    // this makes upper or lower case depending on nullable
    switch (core_data_type) {
      case CQL_DATA_TYPE_INT32:  code += 'I'; break;
      case CQL_DATA_TYPE_INT64:  code += 'L'; break;
      case CQL_DATA_TYPE_DOUBLE: code += 'D'; break;
      case CQL_DATA_TYPE_BOOL:   code += 'F'; bool_count++; break;
      case CQL_DATA_TYPE_STRING: code += 'S'; break;
      case CQL_DATA_TYPE_BLOB:   code += 'B'; break;
    }

    // verifies that we set code
    cql_invariant(code != 0 && code != 'a' - 'A');

    cql_append_value(b, code);
  }

  // null terminate the type info
  code = 0;
  cql_append_value(b, code);

  uint16_t bitvector_bytes_needed = (nullable_count + bool_count + 7) / 8;
  uint8_t *bits = cql_bytebuf_alloc(&b, bitvector_bytes_needed);
  memset(bits, 0, bitvector_bytes_needed);
  uint16_t nullable_index = 0;
  uint16_t bool_index = 0;

  for (uint16_t i = 0; i < count; i++) {
    uint16_t offset = offsets[i+1];
    uint8_t type = types[i];

    int8_t core_data_type = CQL_CORE_DATA_TYPE_OF(type);

    if (type & CQL_DATA_TYPE_NOT_NULL) {
      switch (core_data_type) {
        case CQL_DATA_TYPE_INT32: {
          cql_int32 int32_data = *(cql_int32 *)(cursor + offset);
          cql_write_varint_32(&b, int32_data);
          break;
        }
        case CQL_DATA_TYPE_INT64: {
          cql_int64 int64_data = *(cql_int64 *)(cursor + offset);
          cql_write_varint_64(&b, int64_data);
          break;
        }
        case CQL_DATA_TYPE_DOUBLE: {
          // IEEE 754 big endian seems to be everywhere we need it to be
          // it's good enough for SQLite so it's good enough for us.
          // We're punting on their ARM7 mixed endian support, we don't care about ARM7
          cql_double double_data = *(cql_double *)(cursor + offset);
          cql_append_value(b, double_data);
          break;
        }
        case CQL_DATA_TYPE_BOOL: {
          cql_bool bool_data = *(cql_bool *)(cursor + offset);
          if (bool_data) {
            cql_setbit(bits, nullable_count + bool_index);
          }
          bool_index++;
          break;
        }
        case CQL_DATA_TYPE_STRING: {
          cql_string_ref str_ref = *(cql_string_ref *)(cursor + offset);
          cql_alloc_cstr(temp, str_ref);
          cql_bytebuf_append(&b, temp, (uint32_t)(strlen(temp) + 1));
          cql_free_cstr(temp, str_ref);
          break;
        }
        case CQL_DATA_TYPE_BLOB: {
          cql_blob_ref blob_ref = *(cql_blob_ref *)(cursor + offset);
          const void *bytes = cql_get_blob_bytes(blob_ref);
          cql_uint32 size = cql_get_blob_size(blob_ref);
          cql_append_value(b, size);
          cql_bytebuf_append(&b, bytes, size);
          break;
        }
      }
    }
    else {
      switch (core_data_type) {
        case CQL_DATA_TYPE_INT32: {
          cql_nullable_int32 int32_data = *(cql_nullable_int32 *)(cursor + offset);
          if (!int32_data.is_null) {
            cql_setbit(bits, nullable_index);
            cql_write_varint_32(&b, int32_data.value);
          }
          break;
        }
        case CQL_DATA_TYPE_INT64: {
          cql_nullable_int64 int64_data = *(cql_nullable_int64 *)(cursor + offset);
          if (!int64_data.is_null) {
            cql_setbit(bits, nullable_index);
            cql_write_varint_64(&b, int64_data.value);
          }
          break;
        }
        case CQL_DATA_TYPE_DOUBLE: {
          // IEEE 754 big endian seems to be everywhere we need it to be
          // it's good enough for SQLite so it's good enough for us.
          // We're punting on their ARM7 mixed endian support, we don't care about ARM7
          cql_nullable_double double_data = *(cql_nullable_double *)(cursor + offset);
          cql_append_nullable_value(b, double_data);
          break;
        }
        case CQL_DATA_TYPE_BOOL: {
          cql_nullable_bool bool_data = *(cql_nullable_bool *)(cursor + offset);
          if (!bool_data.is_null) {
            cql_setbit(bits, nullable_index);
            if (bool_data.value) {
              cql_setbit(bits, nullable_count + bool_index);
            }
          }
          bool_index++;
          break;
        }
        case CQL_DATA_TYPE_STRING: {
          cql_string_ref str_ref = *(cql_string_ref *)(cursor + offset);
          if (str_ref) {
            cql_setbit(bits, nullable_index);
            cql_alloc_cstr(temp, str_ref);
            cql_bytebuf_append(&b, temp, (uint32_t)(strlen(temp) + 1));
            cql_free_cstr(temp, str_ref);
          }
          break;
        }
        case CQL_DATA_TYPE_BLOB: {
          cql_blob_ref blob_ref = *(cql_blob_ref *)(cursor + offset);
          if (blob_ref) {
            cql_setbit(bits, nullable_index);
            const void *bytes = cql_get_blob_bytes(blob_ref);
            uint32_t size = cql_get_blob_size(blob_ref);
            cql_append_value(b, size);
            cql_bytebuf_append(&b, bytes, size);
          }
          break;
        }
      }
      nullable_index++;
    }
  }
  cql_invariant(nullable_index == nullable_count);

  cql_blob_ref new_blob = cql_blob_ref_new((const uint8_t *)b.ptr, b.used);
  cql_blob_release(*blob);
  *blob = new_blob;

  cql_bytebuf_close(&b);
  return SQLITE_OK;
}

// Generic method to hash a dynamic cursor:
// Note this code takes advantage of the fact that null valued primitives
// are normalized to "isnull = 1" and "value = 0" so the whole thing can
// be hashed with impunity even when it is in the null state.  With not
// much work this assumption could be removed if needed at a later time.
cql_int64 cql_cursor_hash(cql_dynamic_cursor *_Nonnull dyn_cursor)
{
  if (!*dyn_cursor->cursor_has_row) {
    return 0;
  }

  return (cql_int64)cql_hash_buffer(
    dyn_cursor->cursor_data,
    dyn_cursor->cursor_size,
    dyn_cursor->cursor_refs_count,
    dyn_cursor->cursor_refs_offset);
}

// Generic method to compare two dynamic cursors
// Note this code takes advantage of the fact that null valued primitives
// are normalized to "isnull = 1" and "value = 0" so the whole thing can
// be hashed with impunity even when it is in the null state.  With not
// much work this assumption could be removed if needed at a later time.
cql_bool cql_cursors_equal(cql_dynamic_cursor *_Nonnull c1, cql_dynamic_cursor *_Nonnull c2)
{
  // first check metadata for equivalence, and both must have a row, or not have a row

  if (c1->cursor_size != c2->cursor_size ||
      c1->cursor_refs_count != c2->cursor_refs_count ||
      c1->cursor_refs_offset != c2->cursor_refs_offset ||
      *c1->cursor_has_row != *c2->cursor_has_row) {
    return false;
  }

  // if metadata matches and neither has data that's a match (empty cursors are equal)
  // note we already know their has_row values are the same
  if (!*c1->cursor_has_row) {
    cql_invariant(!*c2->cursor_has_row);
    return true;
  }

  return cql_buffers_equal(
    c1->cursor_data,
    c2->cursor_data,
    c1->cursor_size,
    c1->cursor_refs_count,
    c1->cursor_refs_offset);
}

// release the references in a cursor using the types and offsets info
static void cql_clear_references_before_deserialization(cql_dynamic_cursor *_Nonnull dyn_cursor)
{
  // this is just a normal release of ref columns from the dyn cursor structure
  cql_release_offsets(dyn_cursor->cursor_data, dyn_cursor->cursor_refs_count, dyn_cursor->cursor_refs_offset);
}

#define cql_read_var(buf, var) \
   if (!cql_input_read(buf, &var, sizeof(var))) { \
     goto error; \
   }

cql_code cql_deserialize_from_blob(cql_blob_ref _Nullable b, cql_dynamic_cursor *_Nonnull dyn_cursor)
{
  cql_bool *has_row = dyn_cursor->cursor_has_row;
  uint16_t *offsets = dyn_cursor->cursor_col_offsets;
  uint8_t *types = dyn_cursor->cursor_data_types;
  uint8_t *cursor = dyn_cursor->cursor_data;  // we will be using char offsets

  // we have to release the existing cursor before we start
  // we'll be clobbering the field while we build it.

  *has_row = false;
  cql_clear_references_before_deserialization(dyn_cursor);

  if (!b) {
    goto error;
  }

  const uint8_t *bytes = (const uint8_t *)cql_get_blob_bytes(b);

  cql_input_buf input;
  input.data = bytes;
  input.remaining = cql_get_blob_size(b);

  uint16_t needed_count = offsets[0];  // the first index is the count of fields

  uint16_t nullable_count = 0;
  uint16_t bool_count = 0;
  uint16_t actual_count = 0;


  uint16_t i = 0;

  for (;;) {
    char code;
    cql_read_var(&input, code);

    if (!code) {
      break;
    }

    bool nullable_code = (code >= 'a' && code <= 'z');
    nullable_count += nullable_code;
    actual_count++;

    if (code == 'f' || code == 'F') {
      bool_count++;
    }

    // Extra fields do not have to match, the assumption is that this is
    // a future version of the type talking to a past version.  The past
    // version sees only what it expects to see.  However, we did have
    // to compute the nullable_count and bool_count to get the bit vector
    // size correct.
    if (actual_count <= needed_count) {
      uint8_t type = types[i++];
      bool nullable_type = !(type & CQL_DATA_TYPE_NOT_NULL);
      uint8_t core_data_type = CQL_CORE_DATA_TYPE_OF(type);

      // it's ok if we need a nullable but we're getting a non-nullable
      if (!nullable_type && nullable_code) {
        // nullability must match
        goto error;
      }

      // normalize to the not null type, we've already checked nullability match
      code = nullable_code ? code - ('a' - 'A') : code;

      // ensure that what we have is what we need for all of what we have
      bool code_ok = false;
      switch (core_data_type) {
        case CQL_DATA_TYPE_INT32:  code_ok = code == 'I'; break;
        case CQL_DATA_TYPE_INT64:  code_ok = code == 'L'; break;
        case CQL_DATA_TYPE_DOUBLE: code_ok = code == 'D'; break;
        case CQL_DATA_TYPE_BOOL:   code_ok = code == 'F'; break;
        case CQL_DATA_TYPE_STRING: code_ok = code == 'S'; break;
        case CQL_DATA_TYPE_BLOB:   code_ok = code == 'B'; break;
      }

      if (!code_ok) {
        goto error;
      }
    }
  }

  // if we have too few fields we can use null fillers, this is the versioning
  // policy, we will check that any missing fields are nullable.
  while (i < needed_count) {
    uint8_t type = types[i++];
    if (type & CQL_DATA_TYPE_NOT_NULL) {
      goto error;
    }
  }

  // get the bool bits we need
  const uint8_t *bits;
  uint16_t bytes_needed = (nullable_count + bool_count + 7) / 8;
  if (!cql_input_inline_bytes(&input, &bits, bytes_needed)) {
    goto error;
  }

  uint16_t nullable_index = 0;
  uint16_t bool_index = 0;

  // The types are compatible and we have enough of them, we can start
  // trying to decode.

  for (i = 0; i < needed_count; i++) {
    uint16_t offset = offsets[i+1];
    uint8_t type = types[i];

    cql_int32 core_data_type = CQL_CORE_DATA_TYPE_OF(type);

    bool fetch_data = false;
    bool needed_notnull = !!(type & CQL_DATA_TYPE_NOT_NULL);


    if (i >= actual_count) {
      // we don't have this field
      fetch_data = false;
    }
    else {
      bool actual_notnull = bytes[i] >= 'A' && bytes[i] <= 'Z';

      if (actual_notnull) {
        // marked not null in the metadata means it is always present
        fetch_data = true;
      }
      else {
        // fetch any nullable field if and only if its not null bit is set
        fetch_data = cql_getbit(bits, nullable_index++);
      }
    }

    if (fetch_data) {
      switch (core_data_type) {
        case CQL_DATA_TYPE_INT32: {
          cql_int32 *result;
          if (needed_notnull) {
            result = (cql_int32 *)(cursor + offset);
          }
          else {
            cql_nullable_int32 *nullable_storage = (cql_nullable_int32 *)(cursor+offset);
            nullable_storage->is_null = false;
            result = &nullable_storage->value;
          }
          if (!cql_read_varint_32(&input, result)) {
            goto error;
          }

          break;
        }
        case CQL_DATA_TYPE_INT64: {
          cql_int64 *result;
          if (needed_notnull) {
            result = (cql_int64 *)(cursor + offset);
          }
          else {
            cql_nullable_int64 *nullable_storage = (cql_nullable_int64 *)(cursor+offset);
            nullable_storage->is_null = false;
            result = &nullable_storage->value;
          }
          if (!cql_read_varint_64(&input, result)) {
            goto error;
          }
          break;
        }
        case CQL_DATA_TYPE_DOUBLE: {
          // IEEE 754 big endian seems to be everywhere we need it to be
          // it's good enough for SQLite so it's good enough for us.
          // We're punting on their ARM7 mixed endian support, we don't care about ARM7
          cql_double *result;
          if (needed_notnull) {
            result = (cql_double *)(cursor + offset);
          }
          else {
            cql_nullable_double *nullable_storage = (cql_nullable_double *)(cursor+offset);
            nullable_storage->is_null = false;
            result = &nullable_storage->value;
          }
          cql_read_var(&input, *result);
          break;
        }
        case CQL_DATA_TYPE_BOOL: {
          cql_bool *result;
          if (needed_notnull) {
            result = (cql_bool *)(cursor + offset);
          }
          else {
            cql_nullable_bool *nullable_storage = (cql_nullable_bool *)(cursor+offset);
            nullable_storage->is_null = false;
            result = &nullable_storage->value;
          }
          *result = cql_getbit(bits, nullable_count + bool_index);
          bool_index++;
          break;
        }
        case CQL_DATA_TYPE_STRING: {
          cql_string_ref *str_ref = (cql_string_ref *)(cursor + offset);
          const char *result;
          if (!cql_input_inline_str(&input, &result)) {
            goto error;
          }
          *str_ref = cql_string_ref_new(result);
          break;
        }
        case CQL_DATA_TYPE_BLOB: {
          cql_blob_ref *blob_ref = (cql_blob_ref *)(cursor + offset);
          uint32_t byte_count;
          cql_read_var(&input, byte_count);
          const uint8_t *result;
          if (!cql_input_inline_bytes(&input, &result, byte_count)) {
            goto error;
          }
          *blob_ref = cql_blob_ref_new(result, byte_count);
          break;
        }
      }
    }
    else {
      switch (core_data_type) {
        case CQL_DATA_TYPE_INT32: {
          cql_nullable_int32 *int32_data = (cql_nullable_int32 *)(cursor + offset);
          int32_data->value = 0;
          int32_data->is_null = true;
          break;
        }
        case CQL_DATA_TYPE_INT64: {
          cql_nullable_int64 *int64_data = (cql_nullable_int64 *)(cursor + offset);
          int64_data->value = 0;
          int64_data->is_null = true;
          break;
        }
        case CQL_DATA_TYPE_DOUBLE: {
          cql_nullable_double *double_data = (cql_nullable_double *)(cursor + offset);
          double_data->value = 0;
          double_data->is_null = true;
          break;
        }
        case CQL_DATA_TYPE_BOOL: {
          cql_nullable_bool *bool_data = (cql_nullable_bool *)(cursor + offset);
          bool_data->value = 0;
          bool_data->is_null = true;
          break;
        }
        case CQL_DATA_TYPE_STRING: {
          cql_string_ref *str_ref = (cql_string_ref *)(cursor + offset);
          *str_ref = NULL;
          break;
        }
        case CQL_DATA_TYPE_BLOB: {
          cql_blob_ref *blob_ref = (cql_blob_ref *)(cursor + offset);
          *blob_ref = NULL;
          break;
        }
      }
    }
  }

  *has_row = true;
  return SQLITE_OK;

error:
  *has_row = false;
  cql_clear_references_before_deserialization(dyn_cursor);
  return SQLITE_ERROR;
}

// The outside world does not need to know the details of the partitioning
// so it's defined locally.
typedef struct cql_partition {
  cql_hashtab *_Nonnull ht;
  cql_object_ref _Nullable empty_result; // all empty result sets are the same
  cql_dynamic_cursor c_key; // this captures the shape of the key, all must be the same
  cql_dynamic_cursor c_key2; // two copies of the key shape (for equality checks)
  cql_dynamic_cursor c_val; // row values must also be all the same
  cql_bool has_row; // the stored dynamic cursors above need a has row field, it's here, always true
  cql_bool did_extract; // true if we have begun extracting (no more adding after that)
} cql_partition;

// Any remaining keys should release their references and give back their memory.
// We only have to release if there is at least one reference.
static void cql_partition_key_release(void *_Nullable context, cql_int64 key) {
  cql_partition *_Nonnull self = context;
  void *pv = (void *)key;
  if (self->c_key.cursor_refs_count) {
    cql_release_offsets(pv, self->c_key.cursor_refs_count, self->c_key.cursor_refs_offset);
  }
  free((void *)pv);
}

// We're just going to look at the buffer and release any pointers in any rows
// before releasing the buffer itself.  We only have to do the release operations
// if there was at least one reference in the data.  Otherwise closing the buffer
// releases its internal storage.  The buffer itself doesn't know what it's holding
// so we have to do the internal releases for it.
static void cql_partition_val_release(void *_Nullable context, cql_int64 val) {
  if (val & 1) {
    // this means there is a pre-allocated result set here, we just release it
    cql_object_ref obj = (cql_object_ref)(val & ~(cql_int64)1);
    cql_object_release(obj);
    return;
  }

  cql_partition *_Nonnull self = context;
  cql_bytebuf * buffer = (cql_bytebuf *)val;
  int16_t refs_count = self->c_val.cursor_refs_count;

  if (refs_count) {
    int16_t refs_offset = self->c_val.cursor_refs_offset;
    size_t rowsize = self->c_val.cursor_size;
    int32_t count = buffer->used / rowsize;

    char *row = buffer->ptr;
    for (cql_int32 i = 0; i < count ; i++, row += rowsize) {
      cql_release_offsets(row, refs_count, refs_offset);
    }
  }
  // releases the internal buffer
  cql_bytebuf_close(buffer);
  free(buffer);
}

// When we're going to tear down the partition we want to release anything left in it.
// We just change the release functions now so that they actually do something.  The
// helpers above will free the keys/values including iterating the buffer contents if
// there are any unused buffers left.
static void cql_partition_finalize(void *_Nonnull data) {
  // recover self
  cql_partition *_Nonnull self = data;

  // we're doing final cleanup now so attach the release code
  // these are not ref counted so normally you just copy them (hence no-op retain/release)
  // but now we are doing for real cleanup.
  self->ht->release_key = cql_partition_key_release;
  self->ht->release_val = cql_partition_val_release;

  if (self->empty_result) {
    cql_object_release(self->empty_result);
  }

  cql_hashtab_delete(self->ht);

  free(self);
}

// We just defer to the cursor helper using the stored key metadata
static uint64_t cql_key_cursor_hash(void *_Nullable context, cql_int64 key) {
  cql_contract(context);
  cql_partition *_Nonnull self = context;

  // c_key is preloaded with the unique meta data for this partition
  // all we need to do is copy in the cursor data.  We already verified
  // all metadata is the one and only legal metadata for this partitioning
  self->c_key.cursor_data = (void *)key;
  return cql_cursor_hash(&self->c_key);
}

// We just defer to the cursor helper using the stored key metadata
static bool cql_key_cursor_eq(void *_Nullable context, cql_int64 key1, cql_int64 key2) {
  cql_contract(context);
  cql_partition *_Nonnull self = context;

  // c_key and c_key2 are preloaded with the unique meta data for this partition
  // all we need to do is copy in the cursor data.  We already verified
  // all metadata is the one and only legal metadata for this partitioning
  self->c_key.cursor_data = (void *)key1;
  self->c_key2.cursor_data = (void *)key2;
  return cql_cursors_equal(&self->c_key, &self->c_key2);
}

// This makes an empty partitioning object, which is basically just a configured
// hash table.  The hash table is set to use the helpers above.  Normally there
// is no need to retain/release when rehashing or copying as the hash table is
// the one and only owner of this particular data.  However, we change the
// finalization functions at shutdown to allow the hashtable to help us clean
// up its contents when they are condemned.
cql_object_ref _Nonnull cql_partition_create() {

  cql_partition *_Nonnull self = calloc(1, sizeof(cql_partition));

  cql_object_ref obj = _cql_generic_object_create(self, cql_partition_finalize);

  self->has_row = true;  // we only store cursors with data in them
  self->did_extract = false;  // we haven't yet started extracting

  self->ht = cql_hashtab_new(
      cql_key_cursor_hash,
      cql_key_cursor_eq,
      cql_no_op_retain_release,
      cql_no_op_retain_release,
      cql_no_op_retain_release,
      cql_no_op_retain_release,
      self
    );

  return obj;
}

// This is the main workhorse.  Here the idea is that we are given key columns
// from a particular row as well as the whole row, later we will look up the row
// by its key.  Of course the key doesn't have to be in the row but that's the normal
// pattern.  That is, normally key and val are looking at the same data with key
// being a subset of the columns of val. We are going to hash the key and then
// append the val to a buffer associated with that key.  We make the buffers on
// demand so, there are never really any empty buffers except for a brief instant.
// Any missing keys will have no data.  We use the cursor hashing and equality helpers
// to do the hash table work.  We use the usual retain/release helpers for cursors
// to ensure that the right number of retain/release calls happen on each key/value.
cql_bool cql_partition_cursor(
  cql_object_ref _Nonnull obj,
  cql_dynamic_cursor *_Nonnull key,
  cql_dynamic_cursor *_Nonnull val)
{
  cql_partition *_Nonnull self = _cql_generic_object_get_data(obj);

  // If this contract fails it means you tried to add more rows after extraction began.
  // This is not allowed.  Look up the stack for the invalid call.
  cql_contract(!self->did_extract);

  if (self->c_key.cursor_size) {
    // we're not seeing the first key/val cursor, all copies must be from the same metadata
    cql_contract(self->c_key.cursor_size == key->cursor_size);
    cql_contract(self->c_key.cursor_refs_count == key->cursor_refs_count);
    cql_contract(self->c_key.cursor_refs_offset == key->cursor_refs_offset);
    cql_contract(self->c_val.cursor_size == val->cursor_size);
    cql_contract(self->c_val.cursor_refs_count == val->cursor_refs_count);
    cql_contract(self->c_val.cursor_refs_offset == val->cursor_refs_offset);
  }
  else {
    // we want 2 copies of the metadata for keys (for comparison)
    // one copy of the values shape will do.
    self->c_key = *key;
    self->c_key2 = *key;
    self->c_val = *val;

    // the pointer has to be fixed up to point to the shared (always true) has row
    self->c_key.cursor_has_row = &self->has_row;
    self->c_key2.cursor_has_row = &self->has_row;
    self->c_val.cursor_has_row = &self->has_row;
  }

  if (!*key->cursor_has_row || !*val->cursor_has_row) {
    return false;
  }

  // we want to avoid storing the whole dynamic cursor since they are all the same
  // so we hash on the data and we use the context to get the cursor back
  cql_hashtab_entry *entry = cql_hashtab_find(self->ht, (cql_int64)key->cursor_data);
  cql_bytebuf *buf = NULL;

  if (entry) {
    // we already have a buffer, we can append
    buf = (cql_bytebuf *)entry->val;
  }
  else {
    // create buffer and add to hash table
    buf = malloc(sizeof(*buf));
    cql_bytebuf_open(buf);

    char *k = malloc(key->cursor_size);
    memcpy(k, key->cursor_data, key->cursor_size);
    cql_retain_offsets(k, key->cursor_refs_count, key->cursor_refs_offset);

    cql_bool added = cql_hashtab_add(self->ht, (cql_int64)k, (cql_int64)buf);
    cql_invariant(added);
  }

  cql_invariant(buf);

  // append this value to the growable buffer
  char *new_data = cql_bytebuf_alloc(buf, (int)val->cursor_size);
  memcpy(new_data, val->cursor_data, val->cursor_size);
  cql_retain_offsets(new_data, val->cursor_refs_count, val->cursor_refs_offset);

  return true;
}

// Here we have created partitions previously and we're going to look them up.
// The idea is that if rows for a particular key combo exists then
// we make a result set out of that bunch of rows. If not, we return an
// empty result set (0 rows).  To save space we only create one empty result
// set for all cases in any given partition because all empty results are the same.
cql_object_ref _Nonnull cql_extract_partition(
  cql_object_ref _Nonnull obj,
  cql_dynamic_cursor *_Nonnull key)
{
  cql_partition *_Nonnull self = _cql_generic_object_get_data(obj);

  self->did_extract = true;

  if (self->c_key.cursor_size) {
    cql_contract(self->c_key.cursor_size == key->cursor_size);
    cql_contract(self->c_key.cursor_refs_count == key->cursor_refs_count);
    cql_contract(self->c_key.cursor_refs_offset == key->cursor_refs_offset);
    cql_contract(self->c_val.cursor_size);

    cql_hashtab_entry *entry = cql_hashtab_find(self->ht, (cql_int64)key->cursor_data);
    cql_bytebuf *buf = NULL;

    if (entry) {
      // If we've already computed the value then re-use what we returned before.
      // When used for parent/child processing (the normal case) this would be like
      // having a parent result set where two parent rows refer to the same child result.
      if (entry->val & 1) {
        // strip the lower bit and make the object
        cql_object_ref result_set = (cql_object_ref)(entry->val & ~(cql_int64)1);
        cql_object_retain(result_set);
        return result_set;
      }

      // we have data for this key
      buf = (cql_bytebuf *)entry->val;

      // We always load a valid buffer, if this is zero something very bad has happened.
      cql_invariant(buf);

      cql_int32 count = (cql_int32)(buf->used / self->c_val.cursor_size);

      cql_fetch_info info = {
        .data_types = self->c_val.cursor_data_types,
        .col_offsets = self->c_val.cursor_col_offsets,
        .refs_count = self->c_val.cursor_refs_count,
        .refs_offset = self->c_val.cursor_refs_offset,
        .rowsize = (int32_t)self->c_val.cursor_size,
        .encode_context_index = -1,
      };

      // make the meta from standard info
      cql_result_set_meta meta;
      cql_initialize_meta(&meta, &info);

      void *data = buf->ptr;

      // the bytebuf has been harvested, we can free it now.  We do not "close" it
      // because the result set is taking over the growable buffer, we don't want
      // the buffer to be freed.
      free(buf);

      // retain our copy in case we need it again
      cql_object_ref result = (cql_object_ref)cql_result_set_create(data, count, meta);
      cql_object_retain(result);

      entry->val = 1|(cql_int64)result; // store the result but set the LSB so we know it's not a buffer
      return result;
    }
  }

  if (!self->empty_result) {
    static uint8_t empty_dataTypes[] = { };
    static uint16_t empty_colOffsets[] = { 0 };

    cql_fetch_info empty_info = {
      .data_types = empty_dataTypes,
      .col_offsets = empty_colOffsets,
      .encode_context_index = -1,
    };

    // make the meta from standard info
    cql_result_set_meta empty_meta;
    cql_initialize_meta(&empty_meta, &empty_info);
    self->empty_result = (cql_object_ref)cql_result_set_create(malloc(1), 0, empty_meta);
  }

  cql_invariant(self->empty_result);
  cql_object_retain(self->empty_result);
  return self->empty_result;
}

// Check the table definition "haystack" searching for the column definition string "needle"
// The needle must start after a space or an open paren (start of lexical unit)
// to match a possible column defintion.
cql_bool _cql_contains_column_def(cql_string_ref _Nullable haystack_, cql_string_ref _Nullable needle_) {
  if (!haystack_ || !needle_) {
    return false;
  }

  cql_alloc_cstr(haystack, haystack_);
  cql_alloc_cstr(needle, needle_);

  cql_bool found = false;

  if (!needle[0] || !haystack[0]) {
    goto cleanup;
  }

  const char *p = haystack + 1;

  for (;;) {
    p = strstr(p, needle);
    if (!p) {
      goto cleanup;
    }

    // if column info found at start of word, it's a match
    if (p[-1] == ' ' || p[-1] == '(') {
      found = true;
      goto cleanup;
    }

    p++;
  }

cleanup:

  cql_free_cstr(needle, needle_);
  cql_free_cstr(haystack, haystack_);

  return found;
}

// Defer finalization to the hash table which has all it needs to do the job
static void cql_string_dictionary_finalize(void *_Nonnull data) {
  // recover self
  cql_hashtab *_Nonnull self = data;
  cql_hashtab_delete(self);
}

// This makes a simple string dictionary with retained strings
cql_object_ref _Nonnull cql_string_dictionary_create() {

  // we can re-use the hash, equality, retain, and release from the cql_string_dictionary
  // keys and values are the same in this hash table so we can use the same function
  // to retain/release either
  cql_hashtab *self = cql_hashtab_new(
      cql_key_str_hash,
      cql_key_str_eq,
      cql_key_retain_str,
      cql_key_retain_str,
      cql_key_release_str,
      cql_key_release_str,
      NULL
    );

  cql_object_ref obj = _cql_generic_object_create(self, cql_string_dictionary_finalize);

  return obj;
}

// Delegate the add operation to the internal hashtable
cql_bool cql_string_dictionary_add(
  cql_object_ref _Nonnull dict,
  cql_string_ref _Nonnull key,
  cql_string_ref _Nonnull val)
{
  cql_contract(dict);
  cql_contract(key);
  cql_contract(val);

  cql_hashtab *_Nonnull self = _cql_generic_object_get_data(dict);

  cql_hashtab_entry *entry = cql_hashtab_find(self, (cql_int64)key);

  if (entry) {
    cql_string_retain(val);
    cql_string_release((cql_string_ref)entry->val);
    entry->val = (cql_int64)val;
    return false;
  }

  // retain/release defined above, the key/value will be retained
  return cql_hashtab_add(self, (cql_int64)key, (cql_int64)val);
}

// Lookup the given string in the hash table, note that we do not retain the string
cql_string_ref _Nullable cql_string_dictionary_find(
  cql_object_ref _Nonnull dict,
  cql_string_ref _Nullable key)
{
  cql_contract(dict);

  if (!key) {
     return NULL;
  }

  cql_hashtab *_Nonnull self = _cql_generic_object_get_data(dict);

  cql_hashtab_entry *entry = cql_hashtab_find(self, (cql_int64)key);

  return entry ? (cql_string_ref)entry->val : NULL;
}

// We have to release all the strings in the buffer then release the buffer memory
static void cql_string_list_finalize(void *_Nonnull data) {
  cql_bytebuf *_Nonnull self = data;
  int32_t count = self->used / sizeof(cql_string_ref);
  for (int32_t i = 0; i < count; i++) {
    size_t offset = i * sizeof(cql_string_ref);
    cql_string_ref string = *(cql_string_ref *)(self->ptr + offset);
    cql_string_release(string);
  }
  cql_bytebuf_close(self);
  free(self);
}

// create the list storage using a byte buffer
cql_object_ref _Nonnull cql_string_list_create(void) {

  cql_bytebuf *self = calloc(1, sizeof(cql_bytebuf));
  cql_bytebuf_open(self);
  return _cql_generic_object_create(self, cql_string_list_finalize);
}

// add a string to the buffer and retain it
void cql_string_list_add_string(cql_object_ref _Nullable list, cql_string_ref _Nonnull string) {
  cql_contract(list);
  cql_contract(string);

  cql_string_retain(string);
  cql_bytebuf *_Nonnull self = _cql_generic_object_get_data(list);
  cql_bytebuf_append(self, &string, sizeof(string));
}

// return number of elements
int32_t cql_string_list_get_count(cql_object_ref _Nullable list) {
  int32_t result = 0;

  if (list) {
    cql_bytebuf *_Nonnull self = _cql_generic_object_get_data(list);
    result = self->used / sizeof(cql_string_ref);
  }

  return result;
}

// return the nth string, with no extra retain (get semantics)
cql_string_ref _Nullable cql_string_list_get_string(cql_object_ref _Nullable list, int32_t index) {
  cql_string_ref result = NULL;

  if (list) {
    cql_bytebuf *_Nonnull self = _cql_generic_object_get_data(list);
    int32_t count = self->used / sizeof(cql_string_ref);
    cql_contract(index >= 0 && index < count);
    cql_invariant(self->ptr);
    size_t offset = index * sizeof(cql_string_ref);
    result = *(cql_string_ref *)(self->ptr + offset);
  }

  return result;
}

static void cql_boxed_stmt_finalize(void *_Nonnull data) {
  // note that we use cql_finalize_stmt because it can be and often is
  // intercepted to allow for cql statement pooling.
  sqlite3_stmt *stmt = (sqlite3_stmt *)data;
  cql_finalize_stmt(&stmt);
}

cql_object_ref _Nonnull cql_box_stmt(sqlite3_stmt *_Nullable stmt) {
  return _cql_generic_object_create(stmt, cql_boxed_stmt_finalize);
}

sqlite3_stmt *_Nullable cql_unbox_stmt(cql_object_ref _Nonnull ref) {
  return (sqlite3_stmt *)_cql_generic_object_get_data(ref);
}

// The cursor formatting logic is really super simple
// * we use the bprintf growable buffer format
// * we use the usual dynamic cursor info to find the fields
// * we emit the name of the column and its value
// * if the value is null then we emit the string "null" for its value
// * we put | between fields
// * we use %g for floats
// Note that because we use bprintf we're going to get vsnprintf and not
// the sqlite formatting.  This might be slightly different but the point
// of this method is for diagnostics anyway.  It's already the case that
// floating point formatting can vary between systems and that's really
// where things might be different between runtimes. Making this invariant
// would be pretty costly.  I'm not even sure sqlite printf is invariant
// between systems on that score.
cql_string_ref _Nonnull cql_cursor_format(cql_dynamic_cursor *_Nonnull dyn_cursor)
{
  uint16_t *offsets = dyn_cursor->cursor_col_offsets;
  uint8_t *types = dyn_cursor->cursor_data_types;
  uint16_t count = offsets[0];  // the first index is the count of fields
  uint8_t *cursor = dyn_cursor->cursor_data;  // we will be using char offsets
  const char **fields = dyn_cursor->cursor_fields; // field names for printing

  cql_bytebuf b;
  cql_bytebuf_open(&b);

  for (uint16_t i = 0; i < count; i++) {
    uint16_t offset = offsets[i+1];
    uint8_t type = types[i];
    const char *field = fields[i];

    if (i != 0) {
      cql_bprintf(&b, "|");
    }

    cql_bprintf(&b, "%s:", field);

    int8_t core_data_type = CQL_CORE_DATA_TYPE_OF(type);

    if (type & CQL_DATA_TYPE_NOT_NULL) {
      switch (core_data_type) {
        case CQL_DATA_TYPE_INT32: {
          cql_int32 int32_data = *(cql_int32 *)(cursor + offset);
          cql_bprintf(&b, "%d", int32_data);
          break;
        }
        case CQL_DATA_TYPE_INT64: {
          cql_int64 int64_data = *(cql_int64 *)(cursor + offset);
          cql_bprintf(&b, "%lld", (llint_t)int64_data);
          break;
        }
        case CQL_DATA_TYPE_DOUBLE: {
          cql_double double_data = *(cql_double *)(cursor + offset);
          cql_bprintf(&b, "%g", double_data);
          break;
        }
        case CQL_DATA_TYPE_BOOL: {
          cql_bool bool_data = *(cql_bool *)(cursor + offset);
          cql_bprintf(&b, "%s", bool_data ? "true": "false");
          break;
        }
        case CQL_DATA_TYPE_STRING: {
          cql_string_ref str_ref = *(cql_string_ref *)(cursor + offset);
          cql_alloc_cstr(temp, str_ref);
          cql_bprintf(&b, "%s", temp);
          cql_free_cstr(temp, str_ref);
          break;
        }
        case CQL_DATA_TYPE_BLOB: {
          cql_blob_ref blob_ref = *(cql_blob_ref *)(cursor + offset);
          cql_uint32 size = cql_get_blob_size(blob_ref);
          cql_bprintf(&b, "length %d blob", size);
          break;
        }
      }
    }
    else {
      switch (core_data_type) {
        case CQL_DATA_TYPE_INT32: {
          cql_nullable_int32 int32_data = *(cql_nullable_int32 *)(cursor + offset);
          if (int32_data.is_null) {
            cql_bprintf(&b, "null");
          }
          else {
            cql_bprintf(&b, "%d", int32_data.value);
          }
          break;
        }
        case CQL_DATA_TYPE_INT64: {
          cql_nullable_int64 int64_data = *(cql_nullable_int64 *)(cursor + offset);
          if (int64_data.is_null) {
            cql_bprintf(&b, "null");
          }
          else {
            cql_bprintf(&b, "%lld", (llint_t)int64_data.value);
          }
          break;
        }
        case CQL_DATA_TYPE_DOUBLE: {
          cql_nullable_double double_data = *(cql_nullable_double *)(cursor + offset);
          if (double_data.is_null) {
            cql_bprintf(&b, "null");
          }
          else {
            cql_bprintf(&b, "%g", double_data.value);
          }
          break;
        }
        case CQL_DATA_TYPE_BOOL: {
          cql_nullable_bool bool_data = *(cql_nullable_bool *)(cursor + offset);
          if (bool_data.is_null) {
            cql_bprintf(&b, "null");
          }
          else {
            cql_bprintf(&b, "%s", bool_data.value ? "true" : "false");
          }
          break;
        }
        case CQL_DATA_TYPE_STRING: {
          cql_string_ref str_ref = *(cql_string_ref *)(cursor + offset);
          if (!str_ref) {
            cql_bprintf(&b, "null");
          } else {
            cql_alloc_cstr(temp, str_ref);
            cql_bprintf(&b, "%s", temp);
            cql_free_cstr(temp, str_ref);
          }
          break;
        }
        case CQL_DATA_TYPE_BLOB: {
          cql_blob_ref blob_ref = *(cql_blob_ref *)(cursor + offset);
          if (!blob_ref) {
            cql_bprintf(&b, "null");
          }
          else {
            cql_uint32 size = cql_get_blob_size(blob_ref);
            cql_bprintf(&b, "length %d blob", size);
          }
          break;
        }
      }
    }
  }

  cql_bytebuf_append_null(&b);
  cql_string_ref result = cql_string_ref_new(b.ptr);
  cql_bytebuf_close(&b);
  return result;
}

// To keep the contract as simple as possible we encode everything we
// need into the fragment array.  Including the size of the output
// and fragment terminator.  See above.  This also makes the code
// gen as simple as possible.
cql_string_ref _Nonnull cql_uncompress(const char *_Nonnull base, const char *_Nonnull frags)
{
  // we never try to encode the empty string
  cql_contract(frags[0]);

  // NOTE: len is the allocation size (includes trailing \0)
  int32_t len;
  frags = cql_decode(frags, &len);
  STACK_BYTES_ALLOC(str, len);
  cql_expand_frags(str, base, frags);
  return cql_string_ref_new(str);
}

// This function splits a string by the pattern in parseWord.
// We use this function to listify a series of creates
// (parseWord = "CREATE ") or deletes (parseWord = "DROP")
// after receiving a concatenated string from the CQL upgrader.
// We need some parsing logic with quotes to make sure the parseWord
// is not found inside string literals.
static cql_object_ref _Nonnull _cql_create_upgrader_input_statement_list(cql_string_ref _Nonnull str, char* _Nonnull parse_word)
{
  cql_object_ref list = cql_string_list_create();
  cql_alloc_cstr(c_str, str);
  if (strlen(c_str) == 0) goto cleanup;
  char* lineStart = (char*)(c_str);
  // skip leading whitespace
  while (lineStart[0] == ' '){
    lineStart++;
  }

  // Text has been normalized for SQL so only '' strings no "" strings
  // hence the only escape sequence is ''  e.g.  'That''s all folks'.
  // CQL never generates tabs, formfeeds, or other whitespace except inside
  // quotes, where we already must carefully skip without matching.

  cql_string_ref currLine;
  cql_int32 bytes;

  bool in_quote = false;
  char *p;
  for (p = lineStart; *p; p++) {
    if (in_quote) {
      if (p[0] == '\'') {
        if (p[1] == '\'') {
          p++;
        } else {
          in_quote = false;
        }
      }
    } else if (p[0] == '\'') {
      in_quote = true;
    } else if (!in_quote && !strncmp(p, parse_word, strlen(parse_word))) {
      // Add the current statement (i.e. create statement, drop statement) to our list
      // when we find the delimiting parseWord for the next statement
      if (lineStart != p) {
        bytes = (cql_int32)(p - lineStart);
        char* temp = malloc(bytes + 1);
        memcpy(temp, lineStart, bytes);
        temp[bytes] = '\0';
        currLine = cql_string_ref_new(temp);
        free(temp);
        cql_string_list_add_string(list, currLine);
        cql_string_release(currLine);
        lineStart = p;
      }
    }
  }
  // The last statement is pending because we have been adding statements to the list after seeing
  // the entire statement i.e. beginning of the next statement. We must flush it here.
  bytes = (cql_int32)(p - lineStart);
  char* temp = malloc(bytes + 1);
  memcpy(temp, lineStart, bytes);
  temp[bytes] = '\0';
  currLine = cql_string_ref_new(temp);
  free(temp);
  cql_string_list_add_string(list, currLine);
  cql_string_release(currLine);
cleanup:
  cql_free_cstr(c_str, str);
  return list;
}

// This function assumes the input follows CQL railroad syntax and contains
// characters uptil atleast the first "(" if it exists
static char* _Nonnull _cql_create_table_name_from_table_creation_statement(cql_string_ref _Nonnull create)
{
  char* p;
  // https://cgsql.dev/program-diagram#create_virtual_table_stmt
  // table name always preceeds "USING "
  cql_alloc_cstr(c_create, create);
  if (!strncmp("CREATE VIRTUAL TABLE ", c_create, sizeof("CREATE VIRTUAL TABLE ") - 1)) p = strstr(c_create, "USING ");
  // https://cgsql.dev/program-diagram#create_table_stmt
  // table name always preceeds the first open paren
  else p = strchr(c_create, '(');
  cql_free_cstr(c_create, create);
  // backspace spaces (if they exist) between table name preceeding pattern. We don't
  // want extra spaces in our table names.
  while (p[-1] == ' ') p--;
  char* lineStart = p;
  // find space preceeding table name
  while (lineStart[-1] != ' '){
    lineStart--;
  }
  cql_int32 bytes = (cql_int32)(p - lineStart);
  char* table_name = malloc(bytes + 1);
  memcpy(table_name, lineStart, bytes);
  table_name[bytes] = '\0';
  return table_name;
}

// This function is passed in an index creation statement generated from the CQL upgrader.
// We need this helper to be able to map indices to tables.
static char* _Nonnull _cql_create_table_name_from_index_creation_statement(cql_string_ref _Nonnull index_create)
{
  // table name follows "ON " in the create_index_stmt pattern
  // table name is followed by an open paren
  // https://cgsql.dev/program-diagram#create_index_stmt
  cql_alloc_cstr(c_index_create, index_create);
  char* lineStart = strstr(c_index_create, "ON ") + strlen("ON ");
  cql_free_cstr(c_index_create, index_create);
  char* q = strchr(lineStart, '('); // add space logic
  // backspace spaces between index name and (
  while (q[-1] == ' '){
    q--;
  }
  cql_int32 index_bytes = (cql_int32)(q - lineStart);
  char* index_table_name = malloc(index_bytes + 1);
  memcpy(index_table_name, lineStart, index_bytes);
  index_table_name[index_bytes] = '\0';
  return index_table_name;
}

// This function provides the naive implementation of cql_rebuild_recreate_group called in
// the cg_schema CQL upgrader. We take input three recreate-group specific strings.
// tables: series of semi-colon seperated CREATE (VIRTUAL) TABLE statements
// indices: series of semi-colon seperated CREATE INDEX statements
// deletes: series of semi-colon seperated DROP TABLE statements (ex: unsubscribed or deleted tables)
//
// We currently always do recreate here (no rebuild). We just drop our tables, and recreate the
// tables and any indices that might have been dropped.
cql_code cql_rebuild_recreate_group(sqlite3 *_Nonnull db, cql_string_ref _Nonnull tables, cql_string_ref _Nonnull indices, cql_string_ref _Nonnull deletes, cql_bool *_Nonnull result)
{
  *result = false; // result holds false because we default to recreate (no rebuild)
  // process parseWord separated strings into lists
  cql_object_ref tableList = _cql_create_upgrader_input_statement_list(tables, "CREATE ");
  cql_object_ref indexList = _cql_create_upgrader_input_statement_list(indices, "CREATE ");
  cql_object_ref deleteList = _cql_create_upgrader_input_statement_list(deletes, "DROP ");

  cql_code rc = SQLITE_OK;
  // Execute all delete table drops
  for (cql_int32 i = 0; i < cql_string_list_get_count(deleteList); i++){
    cql_string_ref delete = cql_string_list_get_string(deleteList, i);
    rc = cql_exec_internal(db, delete);
    if (rc != SQLITE_OK) goto cleanup;
  }
  // Execute all table drops based on the list of creates given by the CQL
  // upgrader backwards.
  // Intuitively, need to drop the tables with the most dependencies first.
  for (cql_int32 i = cql_string_list_get_count(tableList) - 1; i >= 0; i--){
    cql_string_ref tableCreate = cql_string_list_get_string(tableList, i);
    char* table_name = _cql_create_table_name_from_table_creation_statement(tableCreate);
    cql_int32 bytes = (cql_int32)(strlen(table_name)) + sizeof("DROP TABLE IF EXISTS ");
    char* drop = malloc(bytes);
    snprintf(drop, bytes, "DROP TABLE IF EXISTS %s", table_name);
    rc = cql_exec(db, drop);
    free(table_name);
    free(drop);
    if (rc != SQLITE_OK) goto cleanup;
  }
  // Execute all table creates in the order provided
  for (cql_int32 i = 0; i < cql_string_list_get_count(tableList); i++){
    cql_string_ref tableCreate = cql_string_list_get_string(tableList, i);
    rc = cql_exec_internal(db, tableCreate);
    if (rc != SQLITE_OK) goto cleanup;
    char* table_name = _cql_create_table_name_from_table_creation_statement(tableCreate);
    // Indices are already deleted with the table drops
    // We need to recreate indices alongside the tables incase future table creates refer to the index
    for (cql_int32 j = 0; j < cql_string_list_get_count(indexList); j++){
      cql_string_ref indexCreate = cql_string_list_get_string(indexList, j);
      char* index_table_name = _cql_create_table_name_from_index_creation_statement(indexCreate);
      if (!strcmp(table_name, index_table_name)) {
        free(index_table_name);
        rc = cql_exec_internal(db, indexCreate);
        if (rc != SQLITE_OK) goto cleanup;
      } else{
        free(index_table_name);
      }
    }
    free(table_name);
  }
  cleanup:
    cql_object_release(tableList);
    cql_object_release(indexList);
    cql_object_release(deleteList);
    return rc;
}
