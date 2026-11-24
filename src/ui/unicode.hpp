#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string_view>

namespace lay::unicode {

// Values correspond to the masks in packaging/generate_unicode.py (tag v1-final).
enum class Break { other, cr, lf, control, extend, zwj, regional, prepend, spacing, l, v, t, lv, lvt };
enum class Indic { none, consonant, extend, linker };

struct Properties {
    unsigned bits;
    Break grapheme() const { return static_cast<Break>(bits & 0xf); }
    Indic indic() const { return static_cast<Indic>((bits >> 4) & 3); }
    bool pictographic() const { return (bits & 0x40) != 0; }
    bool double_width(bool ambiguous_wide) const {
        return (bits & (ambiguous_wide ? 0x380 : 0x180)) != 0;
    }
};

inline Properties properties(std::uint32_t code) {
    if (code >= 0x20 && code < 0x7f) return {0};
    struct Range { std::uint32_t begin, end; unsigned bits; };
    static constexpr Range table[]{
#include "unicode_data.inc"
    };
    const auto range = std::ranges::lower_bound(table, code, {}, &Range::end);
    return {range != std::end(table) && code >= range->begin ? range->bits : 0};
}

// The emulator, not the language of the application, determines ambiguous width.
inline bool cjk_width() {
    static const bool wide = [] {
        const auto* setting = std::getenv("LAY_CJK_WIDTH");
        return setting && std::string_view(setting) == "wide";
    }();
    return wide;
}

class Segmenter {
public:
    bool boundary(Properties next) {
        const auto right = next.grapheme();
        const bool result = breaks(right, next.indic(), next.pictographic());
        pictographic_zwj_ = right == Break::zwj && pictographic_tail_;
        if (right != Break::extend) pictographic_tail_ = next.pictographic();
        switch (next.indic()) {
            case Indic::consonant: indic_consonant_ = true; indic_linker_ = false; break;
            case Indic::linker: indic_linker_ = indic_consonant_; break;
            case Indic::extend: break;
            case Indic::none: indic_consonant_ = indic_linker_ = false; break;
        }
        regional_odd_ = right == Break::regional && !regional_odd_;
        left_ = right;
        return result;
    }

private:
    static bool is_control(Break value) {
        return value == Break::cr || value == Break::lf || value == Break::control;
    }

    // UAX #29 extended grapheme rules GB3 through GB999, in precedence order.
    bool breaks(Break right, Indic indic, bool pictographic) const {
        using enum Break;
        if (left_ == cr && right == lf) return false;
        if (is_control(left_) || is_control(right)) return true;
        if (left_ == l && (right == l || right == v || right == lv || right == lvt)) return false;
        if ((left_ == lv || left_ == v) && (right == v || right == t)) return false;
        if ((left_ == lvt || left_ == t) && right == t) return false;
        if (right == extend || right == zwj || right == spacing || left_ == prepend) return false;
        if (indic == Indic::consonant && indic_linker_) return false;
        if (pictographic && pictographic_zwj_) return false;
        return right != regional || !regional_odd_;
    }

    Break left_ = Break::control;
    bool pictographic_tail_ = false;
    bool pictographic_zwj_ = false;
    bool indic_consonant_ = false;
    bool indic_linker_ = false;
    bool regional_odd_ = false;
};

} // namespace lay::unicode
