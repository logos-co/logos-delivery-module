#pragma once

// Base64 as both plugins need it: message payloads cross the FFI boundary
// base64-encoded, and the signed peer record crosses libp2p_module's JSON
// transport the same way. One implementation, boost.beast's, for both.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <boost/beast/core/detail/base64.hpp>

namespace delivery_base64 {

inline std::string encode(const uint8_t* data, size_t len)
{
    std::string out;
    if (!data || !len) {
        return out;
    }
    namespace b64 = boost::beast::detail::base64;
    out.resize(b64::encoded_size(len));
    out.resize(b64::encode(out.data(), data, len));
    return out;
}

inline std::string encode(const std::vector<uint8_t>& data)
{
    return encode(data.data(), data.size());
}

inline std::vector<uint8_t> decode(const std::string& encoded)
{
    namespace b64 = boost::beast::detail::base64;
    std::vector<uint8_t> out;
    out.resize(b64::decoded_size(encoded.size()));
    auto [written, read] = b64::decode(out.data(), encoded.data(), encoded.size());
    out.resize(written);
    return out;
}

} // namespace delivery_base64
