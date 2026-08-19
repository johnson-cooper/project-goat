#include "DecisionResponse.hpp"

#include <cstring>

namespace goat::ai {

Response encode_type_index(uint16_t type, uint16_t index) {
    Response out{};
    uint32_t r = uint32_t(type) | (uint32_t(index) << 16);
    std::memcpy(out.data(), &r, 4);
    return out;
}

Response encode_raw(int32_t value) {
    Response out{};
    std::memcpy(out.data(), &value, 4);
    return out;
}

Response encode_raw64(uint64_t value) {
    Response out{};
    std::memcpy(out.data(), &value, 8);
    return out;
}

Response encode_place(uint8_t player, uint8_t location, uint8_t sequence) {
    Response out{};
    out[0] = player;
    out[1] = location;
    out[2] = sequence;
    return out;
}

Response encode_indices(const std::vector<uint32_t>& indices) {
    Response out{};
    const auto count = static_cast<uint32_t>(indices.size());
    std::memcpy(out.data() + 4, &count, 4);
    for (uint32_t i = 0; i < count && 8 + i * 4 + 4 <= out.size(); ++i) std::memcpy(out.data() + 8 + i * 4, &indices[i], 4);
    return out;
}

Response encode_first_n(uint32_t count) {
    std::vector<uint32_t> indices;
    indices.reserve(count);
    for (uint32_t i = 0; i < count; ++i) indices.push_back(i);
    return encode_indices(indices);
}

Response encode_counter_amounts(const std::vector<uint16_t>& amounts) {
    Response out{};
    for (size_t i = 0; i < amounts.size() && i * 2 + 2 <= out.size(); ++i) std::memcpy(out.data() + i * 2, &amounts[i], 2);
    return out;
}

Response encode_select_unselect(int32_t action, int32_t index) {
    Response out{};
    std::memcpy(out.data(), &action, 4);
    std::memcpy(out.data() + 4, &index, 4);
    return out;
}

} // namespace goat::ai
