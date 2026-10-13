// Shared pieces of the ParsFlash desktop applications: language, theme and number
// formatting. Qt Widgets only; no engine logic.
#pragma once

#include <string>

#include <QString>

namespace pf::gui {

enum class Lang { En, Fa };

// Dark or light palette on the Fusion style. Safe to call repeatedly.
void apply_theme(bool dark);

QString text_or_dash(const std::string& s);
QString format_rate(double bits_per_second);
QString format_bytes(double bytes);
QString format_latency_us(double microseconds);

}  // namespace pf::gui
