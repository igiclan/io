#include "Kit/UiKit.hpp"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

namespace pf::gui {

void apply_theme(bool dark) {
    qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    if (!dark) {
        qApp->setPalette(qApp->style()->standardPalette());
        return;
    }
    QPalette p;
    const QColor window(30, 30, 34), base(22, 22, 26), alt(36, 36, 42), button(45, 45, 52), text(230, 230, 235),
        highlight(56, 132, 255), disabled(120, 120, 130);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::ToolTipBase, alt);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    qApp->setPalette(p);
}

QString text_or_dash(const std::string& s) { return s.empty() ? QStringLiteral("-") : QString::fromStdString(s); }

QString format_rate(double bps) {
    if (bps >= 1e9) return QStringLiteral("%1 Gb/s").arg(bps / 1e9, 0, 'f', 2);
    if (bps >= 1e6) return QStringLiteral("%1 Mb/s").arg(bps / 1e6, 0, 'f', 2);
    if (bps >= 1e3) return QStringLiteral("%1 Kb/s").arg(bps / 1e3, 0, 'f', 1);
    return QStringLiteral("%1 b/s").arg(bps, 0, 'f', 0);
}

QString format_bytes(double b) {
    if (b >= 1e9) return QStringLiteral("%1 GB").arg(b / 1e9, 0, 'f', 2);
    if (b >= 1e6) return QStringLiteral("%1 MB").arg(b / 1e6, 0, 'f', 2);
    if (b >= 1e3) return QStringLiteral("%1 KB").arg(b / 1e3, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(b, 0, 'f', 0);
}

QString format_latency_us(double us) {
    if (us <= 0) return QStringLiteral("-");
    return QStringLiteral("%1 ms").arg(us / 1000.0, 0, 'f', 2);
}

}  // namespace pf::gui
