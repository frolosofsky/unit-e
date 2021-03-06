// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef UNITE_RPC_UTIL_H
#define UNITE_RPC_UTIL_H

#include <blockchain/blockchain_genesis.h>
#include <blockchain/blockchain_parameters.h>
#include <blockchain/blockchain_types.h>

#include <cstdint>
#include <string>
#include <vector>

#include <univalue.h>

class CKeyStore;
class COutPoint;
class CPubKey;
class CScript;
class CTxIn;
class CTxOut;

namespace staking {
class Coin;
}

CPubKey HexToPubKey(const std::string &hex_in);
CPubKey AddrToPubKey(CKeyStore *keystore, const std::string &addr_in);
CScript CreateMultisigRedeemscript(int required, const std::vector<CPubKey> &pubkeys);

template <typename T>
UniValue ToUniValue(const T &value) {
  return UniValue(value);
}

template <typename T>
UniValue ToUniValue(const std::vector<T> &vector) {
  UniValue array(UniValue::VARR);
  for (const T &v : vector) {
    array.push_back(ToUniValue(v));
  }
  return array;
}

UniValue ToUniValue(std::uint32_t value);
UniValue ToUniValue(std::uint64_t value);
UniValue ToUniValue(float value);
UniValue ToUniValue(double value);
UniValue ToUniValue(const COutPoint &outpoint);
UniValue ToUniValue(const CScript &script);
UniValue ToUniValue(const CTxOut &txout);
UniValue ToUniValue(const CTxIn &txin);
UniValue ToUniValue(const staking::Coin &coin);
UniValue ToUniValue(const uint256 &hash);
UniValue ToUniValue(const blockchain::GenesisBlock &value);
UniValue ToUniValue(const std::vector<unsigned char> base58_prefixes[blockchain::Base58Type::_size_constant]);

#endif  // UNITE_RPC_UTIL_H
