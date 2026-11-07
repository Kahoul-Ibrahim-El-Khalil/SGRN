#pragma once

#include <sgrn/utils/crypto.hpp>

namespace sgrn::utils::keygen
{

using sgrn::utils::crypto::Ed25519KeyPair;
using sgrn::utils::crypto::generateEd25519KeyPair;
using sgrn::utils::crypto::generateRandomBytes;
using sgrn::utils::crypto::generateRandomKeyHex;

} // namespace sgrn::utils::keygen
