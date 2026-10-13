#include "UI/Strings.hpp"

#include <map>
#include <string>

namespace pf::gui {

namespace {

struct Pair {
    const char* en;
    const char* fa;
};

const std::map<std::string, Pair>& table() {
    static const std::map<std::string, Pair> t{
        {"title", {"ParsFlash USB Server", "سرور USB پارس‌فلش"}},
        {"online", {"Online", "آنلاین"}},
        {"offline", {"Offline", "آفلاین"}},
        {"server_name", {"Server", "سرور"}},
        {"address", {"Address", "نشانی"}},
        {"fingerprint", {"Certificate", "گواهی"}},
        {"clients", {"Clients", "کلاینت‌ها"}},
        {"shared_devices", {"Shared devices", "دستگاه‌های به‌اشتراک‌گذاشته"}},
        {"sessions", {"Active sessions", "نشست‌های فعال"}},
        {"upload", {"Upload", "ارسال"}},
        {"download", {"Download", "دریافت"}},
        {"tab_devices", {"USB devices", "دستگاه‌های USB"}},
        {"tab_clients", {"Clients", "کلاینت‌ها"}},
        {"tab_sessions", {"Sessions", "نشست‌ها"}},
        {"col_status", {"Status", "وضعیت"}},
        {"col_device", {"Device", "دستگاه"}},
        {"col_vid", {"VID", "VID"}},
        {"col_pid", {"PID", "PID"}},
        {"col_serial", {"Serial", "سریال"}},
        {"col_speed", {"Speed", "سرعت"}},
        {"col_port", {"Port", "پورت"}},
        {"col_mode", {"Mode", "حالت"}},
        {"col_client", {"Client", "کلاینت"}},
        {"col_traffic", {"Traffic", "ترافیک"}},
        {"col_latency", {"Latency", "تأخیر"}},
        {"col_address", {"Address", "نشانی"}},
        {"col_secure", {"Encrypted", "رمزشده"}},
        {"col_state", {"State", "حالت نشست"}},
        {"col_access", {"Access", "دسترسی"}},
        {"col_inflight", {"In flight", "در جریان"}},
        {"btn_share", {"Share", "اشتراک‌گذاری"}},
        {"btn_unshare", {"Stop sharing", "توقف اشتراک"}},
        {"btn_reset", {"Reset", "ریست"}},
        {"btn_detach", {"Detach", "جداسازی"}},
        {"btn_diag", {"Diagnostics", "عیب‌یابی"}},
        {"language", {"Language", "زبان"}},
        {"theme", {"Theme", "پوسته"}},
        {"theme_dark", {"Dark", "تیره"}},
        {"theme_light", {"Light", "روشن"}},
        {"status_available", {"Available", "در دسترس"}},
        {"status_shared", {"Shared", "به‌اشتراک‌گذاشته"}},
        {"status_in_use", {"In use", "در استفاده"}},
        {"status_unavailable", {"Unavailable", "غیرقابل دسترس"}},
        {"tray_show", {"Show window", "نمایش پنجره"}},
        {"tray_quit", {"Quit", "خروج"}},
        {"no_selection", {"Select a device first.", "ابتدا یک دستگاه انتخاب کنید."}},
        {"confirm_detach", {"Detach this device? The client session will end.",
                            "این دستگاه جدا شود؟ نشست کلاینت پایان می‌یابد."}},
        {"yes", {"Yes", "بله"}},
        {"no", {"No", "خیر"}},
        {"diag_title", {"Diagnostics", "عیب‌یابی"}},
        {"error_title", {"Operation failed", "عملیات ناموفق بود"}},
        {"tls_off", {"TLS disabled (plaintext)", "TLS غیرفعال (بدون رمزنگاری)"}},
        {"none", {"-", "-"}},
    };
    return t;
}

}  // namespace

QString tl(Lang lang, const char* key) {
    const auto& t = table();
    const auto it = t.find(key);
    if (it == t.end()) return QString::fromUtf8(key);
    return QString::fromUtf8(lang == Lang::Fa ? it->second.fa : it->second.en);
}

}  // namespace pf::gui
