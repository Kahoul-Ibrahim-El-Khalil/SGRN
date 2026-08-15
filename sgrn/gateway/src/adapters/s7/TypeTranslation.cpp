#include <sgrn/gateway/adapters/s7/TypeTranslation.hpp>
#include <algorithm>

namespace sgrn::gateway::adapters::s7::TypeTranslation
{

int normalizeServerAreaCode(int t_area_code) {
    switch (t_area_code) {
        case ::S7AreaPE:
            return ::srvAreaPE;
        case ::S7AreaPA:
            return ::srvAreaPA;
        case ::S7AreaMK:
            return ::srvAreaMK;
        case ::S7AreaDB:
            return ::srvAreaDB;
        case ::S7AreaCT:
            return ::srvAreaCT;
        case ::S7AreaTM:
            return ::srvAreaTM;
        default:
            return t_area_code;
    }
}

std::optional<size_t> requestByteSize(const TS7Tag& t_tag) {
    switch (t_tag.WordLen) {
        case S7WLBit:
        case S7WLByte:
        case 0x03:
            return static_cast<size_t>(std::max(1, t_tag.Size));
        case S7WLWord:
        case 0x05:
        case S7WLCounter:
        case S7WLTimer:
            return static_cast<size_t>(std::max(1, t_tag.Size)) * 2U;
        case S7WLDWord:
        case 0x07:
        case S7WLReal:
            return static_cast<size_t>(std::max(1, t_tag.Size)) * 4U;
        default:
            return std::nullopt;
    }
}

} // namespace sgrn::gateway::adapters::s7::TypeTranslation
