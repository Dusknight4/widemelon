// uds_core: the little crypto the UDS service needs (Azahar-compatible beacons): AES-128 (encryption, for
// CTR mode), MD5 and SHA-1. Small and dependency-free, to run on the 3DS as well as on a PC.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace UdsCrypto
{

// AES-128-CTR: data ^= keystream (encrypting and decrypting are the same); ctr is the initial 16-byte
// counter block, incremented big-endian per block (as CryptoPP's CTR_Mode)
void AesCtr(const uint8_t key[16], const uint8_t ctr[16], uint8_t* data, size_t size);

// one AES-128 block (for the self-test)
void AesEncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

void Md5(const uint8_t* data, size_t size, uint8_t out[16]);
void Sha1(const uint8_t* data, size_t size, uint8_t out[20]);

// the standard test vectors; true if all pass
bool SelfTest();

}
