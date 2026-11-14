// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>
namespace VmwareHgfs {
bool Init(const char* root, bool allow_write = false, bool flat_root = false);
void Reset();
bool AddShare(const char* name, const char* root, bool allow_write = false);
void CloseSession(uint16_t session);
std::vector<uint8_t> Request(const std::vector<uint8_t>& request, uint16_t session);
std::vector<uint8_t> Request(const std::vector<uint8_t>& request);
} // namespace VmwareHgfs
