/******************************************************************************
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_FW_API_TESTING_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_FW_API_TESTING_H_

#define FIPS_KEY_LEN_128 16
#define FIPS_KEY_LEN_256 32
#define FIPS_MAX_KEY_LEN FIPS_KEY_LEN_256

#define FIPS_CCM_NONCE_LEN 13
#define FIPS_GCM_NONCE_LEN 12
#define FIPS_MAX_NONCE_LEN FIPS_CCM_NONCE_LEN

#define FIPS_MAX_AAD_LEN 30

/**
 * enum iwl_fips_test_vector_flags - flags for FIPS test vector
 * @IWL_FIPS_TEST_VECTOR_FLAGS_AES: use AES algorithm
 * @IWL_FIPS_TEST_VECTOR_FLAGS_CCM: use CCM algorithm
 * @IWL_FIPS_TEST_VECTOR_FLAGS_GCM: use GCM algorithm
 * @IWL_FIPS_TEST_VECTOR_FLAGS_ENC: if set, the requested operation is
 *  encryption. Otherwise, the requested operation is decryption.
 * @IWL_FIPS_TEST_VECTOR_FLAGS_KEY_256: if set, the test vector uses a
 *  256 bit key. Otherwise 128 bit key is used.
 */
enum iwl_fips_test_vector_flags {
  IWL_FIPS_TEST_VECTOR_FLAGS_AES = BIT(0),
  IWL_FIPS_TEST_VECTOR_FLAGS_CCM = BIT(1),
  IWL_FIPS_TEST_VECTOR_FLAGS_GCM = BIT(2),
  IWL_FIPS_TEST_VECTOR_FLAGS_ENC = BIT(3),
  IWL_FIPS_TEST_VECTOR_FLAGS_KEY_256 = BIT(5),
};

/**
 * struct iwl_fips_test_cmd - FIPS test command for AES/CCM/GCM tests
 * @flags: test vector flags from &enum iwl_fips_test_vector_flags.
 * @payload_len: the length of the @payload field in bytes.
 * @aad_len: the length of the @aad field in bytes.
 * @key: the key used for encryption/decryption. In case a 128-bit key is used,
 *  pad with zero.
 * @aad: AAD. If the AAD is shorter than the buffer, pad with zero. Only valid
 *  for CCM/GCM tests.
 * @reserved: for alignment.
 * @nonce: nonce. Only valid for CCM/GCM tests.
 * @reserved2: for alignment.
 * @payload: the plaintext to encrypt or the cipher text to decrypt + MIC.
 */
struct iwl_fips_test_cmd {
  __le32 flags;
  __le32 payload_len;
  __le32 aad_len;
  uint8_t key[FIPS_MAX_KEY_LEN];
  uint8_t aad[FIPS_MAX_AAD_LEN];
  __le16 reserved;
  uint8_t nonce[FIPS_MAX_NONCE_LEN];
  uint8_t reserved2[3];
  uint8_t payload[0];
} __packed; /* AES_SEC_TEST_VECTOR_HDR_API_S_VER_1 */

/**
 * enum iwl_fips_test_status - FIPS test result status
 * @IWL_FIPS_TEST_STATUS_FAILURE: the requested operation failed.
 * @IWL_FIPS_TEST_STATUS_SUCCESS: the requested operation was completed
 *  successfully. The result buffer is valid.
 */
enum iwl_fips_test_status {
  IWL_FIPS_TEST_STATUS_FAILURE,
  IWL_FIPS_TEST_STATUS_SUCCESS,
};

/**
 * struct iwl_fips_test_resp - FIPS test response
 * @len: the length of the result in bytes.
 * @payload: @len bytes of response followed by status code (uint32_t, one of
 *  &enum iwl_fips_test_status).
 */
struct iwl_fips_test_resp {
  __le32 len;
  uint8_t payload[0];
} __packed; /* AES_SEC_TEST_VECTOR_RESP_API_S_VER_1 */

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_FW_API_TESTING_H_
