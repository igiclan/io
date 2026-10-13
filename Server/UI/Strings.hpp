// UI strings in English and Persian (فارسی). Keys are stable identifiers; a missing
// key returns the key itself so an incomplete translation is visible, not blank.
#pragma once

#include <QString>

namespace pf::gui {

enum class Lang { En, Fa };

QString tl(Lang lang, const char* key);

}  // namespace pf::gui
