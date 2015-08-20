//
// Copyright (C) 2011 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/payload_generator/payload_signer.h"

#include <base/logging.h>
#include <base/strings/string_split.h>
#include <base/strings/string_util.h>
#include <chromeos/data_encoding.h>
#include <openssl/pem.h>

#include "update_engine/omaha_hash_calculator.h"
#include "update_engine/payload_generator/payload_file.h"
#include "update_engine/payload_verifier.h"
#include "update_engine/subprocess.h"
#include "update_engine/update_metadata.pb.h"
#include "update_engine/utils.h"

using std::string;
using std::vector;

namespace chromeos_update_engine {

namespace {

// Given raw |signatures|, packs them into a protobuf and serializes it into a
// binary blob. Returns true on success, false otherwise.
bool ConvertSignatureToProtobufBlob(const vector<chromeos::Blob>& signatures,
                                    chromeos::Blob* out_signature_blob) {
  // Pack it into a protobuf
  Signatures out_message;
  uint32_t version = kSignatureMessageOriginalVersion;
  LOG_IF(WARNING, kSignatureMessageCurrentVersion -
         kSignatureMessageOriginalVersion + 1 < signatures.size())
      << "You may want to support clients in the range ["
      << kSignatureMessageOriginalVersion << ", "
      << kSignatureMessageCurrentVersion << "] inclusive, but you only "
      << "provided " << signatures.size() << " signatures.";
  for (const chromeos::Blob& signature : signatures) {
    Signatures_Signature* sig_message = out_message.add_signatures();
    sig_message->set_version(version++);
    sig_message->set_data(signature.data(), signature.size());
  }

  // Serialize protobuf
  string serialized;
  TEST_AND_RETURN_FALSE(out_message.AppendToString(&serialized));
  out_signature_blob->insert(out_signature_blob->end(),
                             serialized.begin(),
                             serialized.end());
  LOG(INFO) << "Signature blob size: " << out_signature_blob->size();
  return true;
}

// Given an unsigned payload under |payload_path| and the |signature_blob_size|
// generates an updated payload that includes a dummy signature op in its
// manifest. It populates |out_metadata_size| with the size of the final
// manifest after adding the dummy signature operation, and
// |out_signatures_offset| with the expected offset for the new blob. Returns
// true on success, false otherwise.
bool AddSignatureOpToPayload(const string& payload_path,
                             uint64_t signature_blob_size,
                             chromeos::Blob* out_payload,
                             uint64_t* out_metadata_size,
                             uint64_t* out_signatures_offset) {
  const int kProtobufOffset = 20;
  const int kProtobufSizeOffset = 12;

  // Loads the payload.
  chromeos::Blob payload;
  DeltaArchiveManifest manifest;
  uint64_t metadata_size;
  TEST_AND_RETURN_FALSE(PayloadVerifier::LoadPayload(
      payload_path, &payload, &manifest, &metadata_size));

  // Is there already a signature op in place?
  if (manifest.has_signatures_size()) {
    // The signature op is tied to the size of the signature blob, but not it's
    // contents. We don't allow the manifest to change if there is already an op
    // present, because that might invalidate previously generated
    // hashes/signatures.
    if (manifest.signatures_size() != signature_blob_size) {
      LOG(ERROR) << "Attempt to insert different signature sized blob. "
                 << "(current:" << manifest.signatures_size()
                 << "new:" << signature_blob_size << ")";
      return false;
    }

    LOG(INFO) << "Matching signature sizes already present.";
  } else {
    // Updates the manifest to include the signature operation.
    AddSignatureOp(payload.size() - metadata_size,
                   signature_blob_size,
                   &manifest);

    // Updates the payload to include the new manifest.
    string serialized_manifest;
    TEST_AND_RETURN_FALSE(manifest.AppendToString(&serialized_manifest));
    LOG(INFO) << "Updated protobuf size: " << serialized_manifest.size();
    payload.erase(payload.begin() + kProtobufOffset,
                  payload.begin() + metadata_size);
    payload.insert(payload.begin() + kProtobufOffset,
                   serialized_manifest.begin(),
                   serialized_manifest.end());

    // Updates the protobuf size.
    uint64_t size_be = htobe64(serialized_manifest.size());
    memcpy(&payload[kProtobufSizeOffset], &size_be, sizeof(size_be));
    metadata_size = serialized_manifest.size() + kProtobufOffset;

    LOG(INFO) << "Updated payload size: " << payload.size();
    LOG(INFO) << "Updated metadata size: " << metadata_size;
  }

  out_payload->swap(payload);
  *out_metadata_size = metadata_size;
  *out_signatures_offset = metadata_size + manifest.signatures_offset();
  LOG(INFO) << "Signature Blob Offset: " << *out_signatures_offset;
  return true;
}
}  // namespace

bool PayloadSigner::SignHash(const chromeos::Blob& hash,
                             const string& private_key_path,
                             chromeos::Blob* out_signature) {
  LOG(INFO) << "Signing hash with private key: " << private_key_path;
  string sig_path;
  TEST_AND_RETURN_FALSE(
      utils::MakeTempFile("signature.XXXXXX", &sig_path, nullptr));
  ScopedPathUnlinker sig_path_unlinker(sig_path);

  string hash_path;
  TEST_AND_RETURN_FALSE(
      utils::MakeTempFile("hash.XXXXXX", &hash_path, nullptr));
  ScopedPathUnlinker hash_path_unlinker(hash_path);
  // We expect unpadded SHA256 hash coming in
  TEST_AND_RETURN_FALSE(hash.size() == 32);
  chromeos::Blob padded_hash(hash);
  PayloadVerifier::PadRSA2048SHA256Hash(&padded_hash);
  TEST_AND_RETURN_FALSE(utils::WriteFile(hash_path.c_str(),
                                         padded_hash.data(),
                                         padded_hash.size()));

  // This runs on the server, so it's okay to cop out and call openssl
  // executable rather than properly use the library
  vector<string> cmd;
  base::SplitString("openssl rsautl -raw -sign -inkey x -in x -out x",
                    ' ',
                    &cmd);
  cmd[cmd.size() - 5] = private_key_path;
  cmd[cmd.size() - 3] = hash_path;
  cmd[cmd.size() - 1] = sig_path;

  int return_code = 0;
  TEST_AND_RETURN_FALSE(Subprocess::SynchronousExec(cmd, &return_code,
                                                    nullptr));
  TEST_AND_RETURN_FALSE(return_code == 0);

  chromeos::Blob signature;
  TEST_AND_RETURN_FALSE(utils::ReadFile(sig_path, &signature));
  out_signature->swap(signature);
  return true;
}

bool PayloadSigner::SignPayload(const string& unsigned_payload_path,
                                const vector<string>& private_key_paths,
                                chromeos::Blob* out_signature_blob) {
  chromeos::Blob hash_data;
  TEST_AND_RETURN_FALSE(OmahaHashCalculator::RawHashOfFile(
      unsigned_payload_path, -1, &hash_data) ==
                        utils::FileSize(unsigned_payload_path));

  vector<chromeos::Blob> signatures;
  for (const string& path : private_key_paths) {
    chromeos::Blob signature;
    TEST_AND_RETURN_FALSE(SignHash(hash_data, path, &signature));
    signatures.push_back(signature);
  }
  TEST_AND_RETURN_FALSE(ConvertSignatureToProtobufBlob(signatures,
                                                       out_signature_blob));
  return true;
}

bool PayloadSigner::SignatureBlobLength(const vector<string>& private_key_paths,
                                        uint64_t* out_length) {
  DCHECK(out_length);

  string x_path;
  TEST_AND_RETURN_FALSE(
      utils::MakeTempFile("signed_data.XXXXXX", &x_path, nullptr));
  ScopedPathUnlinker x_path_unlinker(x_path);
  TEST_AND_RETURN_FALSE(utils::WriteFile(x_path.c_str(), "x", 1));

  chromeos::Blob sig_blob;
  TEST_AND_RETURN_FALSE(PayloadSigner::SignPayload(x_path,
                                                   private_key_paths,
                                                   &sig_blob));
  *out_length = sig_blob.size();
  return true;
}

bool PayloadSigner::PrepPayloadForHashing(
        const string& payload_path,
        const vector<int>& signature_sizes,
        chromeos::Blob* payload_out,
        uint64_t* metadata_size_out,
        uint64_t* signatures_offset_out) {
  // TODO(petkov): Reduce memory usage -- the payload is manipulated in memory.

  // Loads the payload and adds the signature op to it.
  vector<chromeos::Blob> signatures;
  for (int signature_size : signature_sizes) {
    signatures.emplace_back(signature_size, 0);
  }
  chromeos::Blob signature_blob;
  TEST_AND_RETURN_FALSE(ConvertSignatureToProtobufBlob(signatures,
                                                       &signature_blob));
  TEST_AND_RETURN_FALSE(AddSignatureOpToPayload(payload_path,
                                                signature_blob.size(),
                                                payload_out,
                                                metadata_size_out,
                                                signatures_offset_out));

  return true;
}

bool PayloadSigner::HashPayloadForSigning(const string& payload_path,
                                          const vector<int>& signature_sizes,
                                          chromeos::Blob* out_hash_data) {
  chromeos::Blob payload;
  uint64_t metadata_size;
  uint64_t signatures_offset;

  TEST_AND_RETURN_FALSE(PrepPayloadForHashing(payload_path,
                                              signature_sizes,
                                              &payload,
                                              &metadata_size,
                                              &signatures_offset));

  // Calculates the hash on the updated payload. Note that we stop calculating
  // before we reach the signature information.
  TEST_AND_RETURN_FALSE(OmahaHashCalculator::RawHashOfBytes(payload.data(),
                                                            signatures_offset,
                                                            out_hash_data));
  return true;
}

bool PayloadSigner::HashMetadataForSigning(const string& payload_path,
                                           const vector<int>& signature_sizes,
                                           chromeos::Blob* out_metadata_hash) {
  chromeos::Blob payload;
  uint64_t metadata_size;
  uint64_t signatures_offset;

  TEST_AND_RETURN_FALSE(PrepPayloadForHashing(payload_path,
                                              signature_sizes,
                                              &payload,
                                              &metadata_size,
                                              &signatures_offset));

  // Calculates the hash on the manifest.
  TEST_AND_RETURN_FALSE(OmahaHashCalculator::RawHashOfBytes(payload.data(),
                                                            metadata_size,
                                                            out_metadata_hash));
  return true;
}

bool PayloadSigner::AddSignatureToPayload(
    const string& payload_path,
    const vector<chromeos::Blob>& signatures,
    const string& signed_payload_path,
    uint64_t *out_metadata_size) {
  // TODO(petkov): Reduce memory usage -- the payload is manipulated in memory.

  // Loads the payload and adds the signature op to it.
  chromeos::Blob signature_blob;
  TEST_AND_RETURN_FALSE(ConvertSignatureToProtobufBlob(signatures,
                                                       &signature_blob));
  chromeos::Blob payload;
  uint64_t signatures_offset;
  TEST_AND_RETURN_FALSE(AddSignatureOpToPayload(payload_path,
                                                signature_blob.size(),
                                                &payload,
                                                out_metadata_size,
                                                &signatures_offset));
  // Appends the signature blob to the end of the payload and writes the new
  // payload.
  LOG(INFO) << "Payload size before signatures: " << payload.size();
  payload.resize(signatures_offset);
  payload.insert(payload.begin() + signatures_offset,
                 signature_blob.begin(),
                 signature_blob.end());
  LOG(INFO) << "Signed payload size: " << payload.size();
  TEST_AND_RETURN_FALSE(utils::WriteFile(signed_payload_path.c_str(),
                                         payload.data(),
                                         payload.size()));
  return true;
}

bool PayloadSigner::GetMetadataSignature(const void* const metadata,
                                         size_t metadata_size,
                                         const string& private_key_path,
                                         string* out_signature) {
  // Calculates the hash on the updated payload. Note that the payload includes
  // the signature op but doesn't include the signature blob at the end.
  chromeos::Blob metadata_hash;
  TEST_AND_RETURN_FALSE(OmahaHashCalculator::RawHashOfBytes(metadata,
                                                            metadata_size,
                                                            &metadata_hash));

  chromeos::Blob signature;
  TEST_AND_RETURN_FALSE(SignHash(metadata_hash,
                                 private_key_path,
                                 &signature));

  *out_signature = chromeos::data_encoding::Base64Encode(signature);
  return true;
}


}  // namespace chromeos_update_engine
