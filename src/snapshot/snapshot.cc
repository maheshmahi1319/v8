// Copyright 2006-2008 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The common functionality when building with or without snapshots.

#include "src/snapshot/snapshot.h"

#include "src/base/platform/platform.h"
#include "src/logging/counters.h"
#include "src/snapshot/partial-deserializer.h"
#include "src/snapshot/read-only-deserializer.h"
#include "src/snapshot/snapshot-utils.h"
#include "src/snapshot/startup-deserializer.h"
#include "src/utils/memcopy.h"
#include "src/utils/version.h"

#ifdef V8_SNAPSHOT_COMPRESSION
#include "src/snapshot/snapshot-compression.h"
#endif

namespace v8 {
namespace internal {

namespace {

class SnapshotImpl : public AllStatic {
 public:
  static uint32_t ExtractNumContexts(const v8::StartupData* data);
  static uint32_t ExtractContextOffset(const v8::StartupData* data,
                                       uint32_t index);
  static Vector<const byte> ExtractStartupData(const v8::StartupData* data);
  static Vector<const byte> ExtractReadOnlyData(const v8::StartupData* data);
  static Vector<const byte> ExtractContextData(const v8::StartupData* data,
                                               uint32_t index);

  static uint32_t GetHeaderValue(const v8::StartupData* data, uint32_t offset) {
    return base::ReadLittleEndianValue<uint32_t>(
        reinterpret_cast<Address>(data->data) + offset);
  }
  static void SetHeaderValue(char* data, uint32_t offset, uint32_t value) {
    base::WriteLittleEndianValue(reinterpret_cast<Address>(data) + offset,
                                 value);
  }

  static void CheckVersion(const v8::StartupData* data);

  // Snapshot blob layout:
  // [0] number of contexts N
  // [1] rehashability
  // [2] checksum
  // [3] (128 bytes) version string
  // [4] offset to readonly
  // [5] offset to context 0
  // [6] offset to context 1
  // ...
  // ... offset to context N - 1
  // ... startup snapshot data
  // ... read-only snapshot data
  // ... context 0 snapshot data
  // ... context 1 snapshot data

  static const uint32_t kNumberOfContextsOffset = 0;
  // TODO(yangguo): generalize rehashing, and remove this flag.
  static const uint32_t kRehashabilityOffset =
      kNumberOfContextsOffset + kUInt32Size;
  static const uint32_t kChecksumOffset = kRehashabilityOffset + kUInt32Size;
  static const uint32_t kVersionStringOffset = kChecksumOffset + kUInt32Size;
  static const uint32_t kVersionStringLength = 64;
  static const uint32_t kReadOnlyOffsetOffset =
      kVersionStringOffset + kVersionStringLength;
  static const uint32_t kFirstContextOffsetOffset =
      kReadOnlyOffsetOffset + kUInt32Size;

  static Vector<const byte> ChecksummedContent(const v8::StartupData* data) {
    STATIC_ASSERT(kVersionStringOffset == kChecksumOffset + kUInt32Size);
    const uint32_t kChecksumStart = kVersionStringOffset;
    return Vector<const byte>(
        reinterpret_cast<const byte*>(data->data + kChecksumStart),
        data->raw_size - kChecksumStart);
  }

  static uint32_t StartupSnapshotOffset(int num_contexts) {
    return POINTER_SIZE_ALIGN(kFirstContextOffsetOffset +
                              num_contexts * kInt32Size);
  }

  static uint32_t ContextSnapshotOffsetOffset(int index) {
    return kFirstContextOffsetOffset + index * kInt32Size;
  }
};

}  // namespace

SnapshotData MaybeDecompress(const Vector<const byte>& snapshot_data) {
#ifdef V8_SNAPSHOT_COMPRESSION
  return SnapshotCompression::Decompress(snapshot_data);
#else
  return SnapshotData(snapshot_data);
#endif
}

#ifdef DEBUG
bool Snapshot::SnapshotIsValid(const v8::StartupData* snapshot_blob) {
  return SnapshotImpl::ExtractNumContexts(snapshot_blob) > 0;
}
#endif  // DEBUG

bool Snapshot::HasContextSnapshot(Isolate* isolate, size_t index) {
  // Do not use snapshots if the isolate is used to create snapshots.
  const v8::StartupData* blob = isolate->snapshot_blob();
  if (blob == nullptr) return false;
  if (blob->data == nullptr) return false;
  size_t num_contexts =
      static_cast<size_t>(SnapshotImpl::ExtractNumContexts(blob));
  return index < num_contexts;
}

bool Snapshot::Initialize(Isolate* isolate) {
  if (!isolate->snapshot_available()) return false;
  RuntimeCallTimerScope rcs_timer(isolate,
                                  RuntimeCallCounterId::kDeserializeIsolate);
  base::ElapsedTimer timer;
  if (FLAG_profile_deserialization) timer.Start();

  const v8::StartupData* blob = isolate->snapshot_blob();
  SnapshotImpl::CheckVersion(blob);
  CHECK(VerifyChecksum(blob));
  Vector<const byte> startup_data = SnapshotImpl::ExtractStartupData(blob);
  Vector<const byte> read_only_data = SnapshotImpl::ExtractReadOnlyData(blob);

  SnapshotData startup_snapshot_data(MaybeDecompress(startup_data));
  SnapshotData read_only_snapshot_data(MaybeDecompress(read_only_data));

  StartupDeserializer startup_deserializer(&startup_snapshot_data);
  ReadOnlyDeserializer read_only_deserializer(&read_only_snapshot_data);
  startup_deserializer.SetRehashability(ExtractRehashability(blob));
  read_only_deserializer.SetRehashability(ExtractRehashability(blob));
  bool success =
      isolate->InitWithSnapshot(&read_only_deserializer, &startup_deserializer);
  if (FLAG_profile_deserialization) {
    double ms = timer.Elapsed().InMillisecondsF();
    int bytes = startup_data.length();
    PrintF("[Deserializing isolate (%d bytes) took %0.3f ms]\n", bytes, ms);
  }
  return success;
}

MaybeHandle<Context> Snapshot::NewContextFromSnapshot(
    Isolate* isolate, Handle<JSGlobalProxy> global_proxy, size_t context_index,
    v8::DeserializeEmbedderFieldsCallback embedder_fields_deserializer) {
  if (!isolate->snapshot_available()) return Handle<Context>();
  RuntimeCallTimerScope rcs_timer(isolate,
                                  RuntimeCallCounterId::kDeserializeContext);
  base::ElapsedTimer timer;
  if (FLAG_profile_deserialization) timer.Start();

  const v8::StartupData* blob = isolate->snapshot_blob();
  bool can_rehash = ExtractRehashability(blob);
  Vector<const byte> context_data = SnapshotImpl::ExtractContextData(
      blob, static_cast<uint32_t>(context_index));
  SnapshotData snapshot_data(MaybeDecompress(context_data));

  MaybeHandle<Context> maybe_result = ContextDeserializer::DeserializeContext(
      isolate, &snapshot_data, can_rehash, global_proxy,
      embedder_fields_deserializer);

  Handle<Context> result;
  if (!maybe_result.ToHandle(&result)) return MaybeHandle<Context>();

  if (FLAG_profile_deserialization) {
    double ms = timer.Elapsed().InMillisecondsF();
    int bytes = context_data.length();
    PrintF("[Deserializing context #%zu (%d bytes) took %0.3f ms]\n",
           context_index, bytes, ms);
  }
  return result;
}

void ProfileDeserialization(
    const SnapshotData* read_only_snapshot,
    const SnapshotData* startup_snapshot,
    const std::vector<SnapshotData*>& context_snapshots) {
  if (FLAG_profile_deserialization) {
    int startup_total = 0;
    PrintF("Deserialization will reserve:\n");
    for (const auto& reservation : read_only_snapshot->Reservations()) {
      startup_total += reservation.chunk_size();
    }
    for (const auto& reservation : startup_snapshot->Reservations()) {
      startup_total += reservation.chunk_size();
    }
    PrintF("%10d bytes per isolate\n", startup_total);
    for (size_t i = 0; i < context_snapshots.size(); i++) {
      int context_total = 0;
      for (const auto& reservation : context_snapshots[i]->Reservations()) {
        context_total += reservation.chunk_size();
      }
      PrintF("%10d bytes per context #%zu\n", context_total, i);
    }
  }
}

v8::StartupData Snapshot::CreateSnapshotBlob(
    const SnapshotData* startup_snapshot_in,
    const SnapshotData* read_only_snapshot_in,
    const std::vector<SnapshotData*>& context_snapshots_in,
    bool can_be_rehashed) {
  // Have these separate from snapshot_in for compression, since we need to
  // access the compressed data as well as the uncompressed reservations.
  const SnapshotData* startup_snapshot;
  const SnapshotData* read_only_snapshot;
  const std::vector<SnapshotData*>* context_snapshots;
#ifdef V8_SNAPSHOT_COMPRESSION
  SnapshotData startup_compressed(
      SnapshotCompression::Compress(startup_snapshot_in));
  SnapshotData read_only_compressed(
      SnapshotCompression::Compress(read_only_snapshot_in));
  startup_snapshot = &startup_compressed;
  read_only_snapshot = &read_only_compressed;
  std::vector<SnapshotData> context_snapshots_compressed;
  context_snapshots_compressed.reserve(context_snapshots_in.size());
  std::vector<SnapshotData*> context_snapshots_compressed_ptrs;
  for (unsigned int i = 0; i < context_snapshots_in.size(); ++i) {
    context_snapshots_compressed.push_back(
        SnapshotCompression::Compress(context_snapshots_in[i]));
    context_snapshots_compressed_ptrs.push_back(
        &context_snapshots_compressed[i]);
  }
  context_snapshots = &context_snapshots_compressed_ptrs;
#else
  startup_snapshot = startup_snapshot_in;
  read_only_snapshot = read_only_snapshot_in;
  context_snapshots = &context_snapshots_in;
#endif

  uint32_t num_contexts = static_cast<uint32_t>(context_snapshots->size());
  uint32_t startup_snapshot_offset =
      SnapshotImpl::StartupSnapshotOffset(num_contexts);
  uint32_t total_length = startup_snapshot_offset;
  total_length += static_cast<uint32_t>(startup_snapshot->RawData().length());
  total_length += static_cast<uint32_t>(read_only_snapshot->RawData().length());
  for (const auto context_snapshot : *context_snapshots) {
    total_length += static_cast<uint32_t>(context_snapshot->RawData().length());
  }

  ProfileDeserialization(read_only_snapshot_in, startup_snapshot_in,
                         context_snapshots_in);

  char* data = new char[total_length];
  // Zero out pre-payload data. Part of that is only used for padding.
  memset(data, 0, SnapshotImpl::StartupSnapshotOffset(num_contexts));

  SnapshotImpl::SetHeaderValue(data, SnapshotImpl::kNumberOfContextsOffset,
                               num_contexts);
  SnapshotImpl::SetHeaderValue(data, SnapshotImpl::kRehashabilityOffset,
                               can_be_rehashed ? 1 : 0);

  // Write version string into snapshot data.
  memset(data + SnapshotImpl::kVersionStringOffset, 0,
         SnapshotImpl::kVersionStringLength);
  Version::GetString(Vector<char>(data + SnapshotImpl::kVersionStringOffset,
                                  SnapshotImpl::kVersionStringLength));

  // Startup snapshot (isolate-specific data).
  uint32_t payload_offset = startup_snapshot_offset;
  uint32_t payload_length =
      static_cast<uint32_t>(startup_snapshot->RawData().length());
  CopyBytes(data + payload_offset,
            reinterpret_cast<const char*>(startup_snapshot->RawData().begin()),
            payload_length);
  if (FLAG_profile_deserialization) {
    PrintF("Snapshot blob consists of:\n%10d bytes in %d chunks for startup\n",
           payload_length,
           static_cast<uint32_t>(startup_snapshot_in->Reservations().size()));
  }
  payload_offset += payload_length;

  // Read-only.
  SnapshotImpl::SetHeaderValue(data, SnapshotImpl::kReadOnlyOffsetOffset,
                               payload_offset);
  payload_length = read_only_snapshot->RawData().length();
  CopyBytes(
      data + payload_offset,
      reinterpret_cast<const char*>(read_only_snapshot->RawData().begin()),
      payload_length);
  if (FLAG_profile_deserialization) {
    PrintF("%10d bytes for read-only\n", payload_length);
  }
  payload_offset += payload_length;

  // Context snapshots (context-specific data).
  for (uint32_t i = 0; i < num_contexts; i++) {
    SnapshotImpl::SetHeaderValue(
        data, SnapshotImpl::ContextSnapshotOffsetOffset(i), payload_offset);
    SnapshotData* context_snapshot = (*context_snapshots)[i];
    payload_length = context_snapshot->RawData().length();
    CopyBytes(
        data + payload_offset,
        reinterpret_cast<const char*>(context_snapshot->RawData().begin()),
        payload_length);
    if (FLAG_profile_deserialization) {
      PrintF(
          "%10d bytes in %d chunks for context #%d\n", payload_length,
          static_cast<uint32_t>(context_snapshots_in[i]->Reservations().size()),
          i);
    }
    payload_offset += payload_length;
  }

  DCHECK_EQ(total_length, payload_offset);
  v8::StartupData result = {data, static_cast<int>(total_length)};

  SnapshotImpl::SetHeaderValue(
      data, SnapshotImpl::kChecksumOffset,
      Checksum(SnapshotImpl::ChecksummedContent(&result)));

  return result;
}

uint32_t SnapshotImpl::ExtractNumContexts(const v8::StartupData* data) {
  CHECK_LT(kNumberOfContextsOffset, data->raw_size);
  uint32_t num_contexts = GetHeaderValue(data, kNumberOfContextsOffset);
  return num_contexts;
}

bool Snapshot::VerifyChecksum(const v8::StartupData* data) {
  base::ElapsedTimer timer;
  if (FLAG_profile_deserialization) timer.Start();
  uint32_t expected =
      SnapshotImpl::GetHeaderValue(data, SnapshotImpl::kChecksumOffset);
  uint32_t result = Checksum(SnapshotImpl::ChecksummedContent(data));
  if (FLAG_profile_deserialization) {
    double ms = timer.Elapsed().InMillisecondsF();
    PrintF("[Verifying snapshot checksum took %0.3f ms]\n", ms);
  }
  return result == expected;
}

uint32_t SnapshotImpl::ExtractContextOffset(const v8::StartupData* data,
                                            uint32_t index) {
  // Extract the offset of the context at a given index from the StartupData,
  // and check that it is within bounds.
  uint32_t context_offset =
      GetHeaderValue(data, ContextSnapshotOffsetOffset(index));
  CHECK_LT(context_offset, static_cast<uint32_t>(data->raw_size));
  return context_offset;
}

bool Snapshot::ExtractRehashability(const v8::StartupData* data) {
  CHECK_LT(SnapshotImpl::kRehashabilityOffset,
           static_cast<uint32_t>(data->raw_size));
  uint32_t rehashability =
      SnapshotImpl::GetHeaderValue(data, SnapshotImpl::kRehashabilityOffset);
  CHECK_IMPLIES(rehashability != 0, rehashability == 1);
  return rehashability != 0;
}

namespace {
Vector<const byte> ExtractData(const v8::StartupData* snapshot,
                               uint32_t start_offset, uint32_t end_offset) {
  CHECK_LT(start_offset, end_offset);
  CHECK_LT(end_offset, snapshot->raw_size);
  uint32_t length = end_offset - start_offset;
  const byte* data =
      reinterpret_cast<const byte*>(snapshot->data + start_offset);
  return Vector<const byte>(data, length);
}
}  // namespace

Vector<const byte> SnapshotImpl::ExtractStartupData(
    const v8::StartupData* data) {
  DCHECK(Snapshot::SnapshotIsValid(data));

  uint32_t num_contexts = ExtractNumContexts(data);
  return ExtractData(data, StartupSnapshotOffset(num_contexts),
                     GetHeaderValue(data, kReadOnlyOffsetOffset));
}

Vector<const byte> SnapshotImpl::ExtractReadOnlyData(
    const v8::StartupData* data) {
  DCHECK(Snapshot::SnapshotIsValid(data));

  return ExtractData(data, GetHeaderValue(data, kReadOnlyOffsetOffset),
                     GetHeaderValue(data, ContextSnapshotOffsetOffset(0)));
}

Vector<const byte> SnapshotImpl::ExtractContextData(const v8::StartupData* data,
                                                    uint32_t index) {
  uint32_t num_contexts = ExtractNumContexts(data);
  CHECK_LT(index, num_contexts);

  uint32_t context_offset = ExtractContextOffset(data, index);
  uint32_t next_context_offset;
  if (index == num_contexts - 1) {
    next_context_offset = data->raw_size;
  } else {
    next_context_offset = ExtractContextOffset(data, index + 1);
    CHECK_LT(next_context_offset, data->raw_size);
  }

  const byte* context_data =
      reinterpret_cast<const byte*>(data->data + context_offset);
  uint32_t context_length = next_context_offset - context_offset;
  return Vector<const byte>(context_data, context_length);
}

void SnapshotImpl::CheckVersion(const v8::StartupData* data) {
  char version[kVersionStringLength];
  memset(version, 0, kVersionStringLength);
  CHECK_LT(kVersionStringOffset + kVersionStringLength,
           static_cast<uint32_t>(data->raw_size));
  Version::GetString(Vector<char>(version, kVersionStringLength));
  if (strncmp(version, data->data + kVersionStringOffset,
              kVersionStringLength) != 0) {
    FATAL(
        "Version mismatch between V8 binary and snapshot.\n"
        "#   V8 binary version: %.*s\n"
        "#    Snapshot version: %.*s\n"
        "# The snapshot consists of %d bytes and contains %d context(s).",
        kVersionStringLength, version, kVersionStringLength,
        data->data + kVersionStringOffset, data->raw_size,
        ExtractNumContexts(data));
  }
}

namespace {

bool RunExtraCode(v8::Isolate* isolate, v8::Local<v8::Context> context,
                  const char* utf8_source, const char* name) {
  v8::Context::Scope context_scope(context);
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> source_string;
  if (!v8::String::NewFromUtf8(isolate, utf8_source).ToLocal(&source_string)) {
    return false;
  }
  v8::Local<v8::String> resource_name =
      v8::String::NewFromUtf8(isolate, name).ToLocalChecked();
  v8::ScriptOrigin origin(resource_name);
  v8::ScriptCompiler::Source source(source_string, origin);
  v8::Local<v8::Script> script;
  if (!v8::ScriptCompiler::Compile(context, &source).ToLocal(&script))
    return false;
  if (script->Run(context).IsEmpty()) return false;
  CHECK(!try_catch.HasCaught());
  return true;
}

}  // namespace

v8::StartupData CreateSnapshotDataBlobInternal(
    v8::SnapshotCreator::FunctionCodeHandling function_code_handling,
    const char* embedded_source, v8::Isolate* isolate) {
  // If no isolate is passed in, create it (and a new context) from scratch.
  if (isolate == nullptr) isolate = v8::Isolate::Allocate();

  // Optionally run a script to embed, and serialize to create a snapshot blob.
  v8::SnapshotCreator snapshot_creator(isolate);
  {
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    if (embedded_source != nullptr &&
        !RunExtraCode(isolate, context, embedded_source, "<embedded>")) {
      return {};
    }
    snapshot_creator.SetDefaultContext(context);
  }
  return snapshot_creator.CreateBlob(function_code_handling);
}

v8::StartupData WarmUpSnapshotDataBlobInternal(
    v8::StartupData cold_snapshot_blob, const char* warmup_source) {
  CHECK(cold_snapshot_blob.raw_size > 0 && cold_snapshot_blob.data != nullptr);
  CHECK_NOT_NULL(warmup_source);

  // Use following steps to create a warmed up snapshot blob from a cold one:
  //  - Create a new isolate from the cold snapshot.
  //  - Create a new context to run the warmup script. This will trigger
  //    compilation of executed functions.
  //  - Create a new context. This context will be unpolluted.
  //  - Serialize the isolate and the second context into a new snapshot blob.
  v8::SnapshotCreator snapshot_creator(nullptr, &cold_snapshot_blob);
  v8::Isolate* isolate = snapshot_creator.GetIsolate();
  {
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    if (!RunExtraCode(isolate, context, warmup_source, "<warm-up>")) {
      return {};
    }
  }
  {
    v8::HandleScope handle_scope(isolate);
    isolate->ContextDisposedNotification(false);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    snapshot_creator.SetDefaultContext(context);
  }

  return snapshot_creator.CreateBlob(
      v8::SnapshotCreator::FunctionCodeHandling::kKeep);
}

}  // namespace internal
}  // namespace v8
