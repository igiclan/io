#include "UI/ClientStrings.hpp"

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
        {"title", {"ParsFlash USB Client", "کلاینت USB پارس‌فلش"}},
        {"test_driver", {"TEST DRIVER: devices are not visible to Windows applications",
                         "درایور آزمایشی: دستگاه‌ها برای برنامه‌های ویندوز دیده نمی‌شوند"}},
        {"client_name", {"This PC", "این رایانه"}},
        {"vhci_ok", {"Virtual USB controller: ready", "کنترلر USB مجازی: آماده"}},
        {"vhci_missing", {"Virtual USB controller: unavailable", "کنترلر USB مجازی: در دسترس نیست"}},
        {"sessions_count", {"Sessions", "نشست‌ها"}},
        {"servers", {"Servers", "سرورها"}},
        {"devices", {"Remote devices", "دستگاه‌های راه‌دور"}},
        {"sessions", {"Sessions", "نشست‌ها"}},
        {"col_server", {"Server", "سرور"}},
        {"col_address", {"Address", "نشانی"}},
        {"col_state", {"State", "وضعیت"}},
        {"col_latency", {"Latency", "تأخیر"}},
        {"col_upload", {"Upload", "ارسال"}},
        {"col_download", {"Download", "دریافت"}},
        {"col_reconnects", {"Reconnects", "اتصال مجدد"}},
        {"col_sessions", {"Sessions", "نشست‌ها"}},
        {"col_status", {"Status", "وضعیت"}},
        {"col_device", {"Device", "دستگاه"}},
        {"col_vid", {"VID", "VID"}},
        {"col_pid", {"PID", "PID"}},
        {"col_serial", {"Serial", "سریال"}},
        {"col_speed", {"Speed", "سرعت"}},
        {"col_port", {"Port", "پورت"}},
        {"col_mode", {"Mode", "حالت"}},
        {"col_access", {"Access", "دسترسی"}},
        {"col_handle", {"Virtual port", "پورت مجازی"}},
        {"col_inflight", {"In flight", "در جریان"}},
        {"col_held", {"Held", "نگه‌داشته"}},
        {"col_traffic", {"Traffic", "ترافیک"}},
        {"btn_add_server", {"Add server", "افزودن سرور"}},
        {"btn_remove_server", {"Remove", "حذف"}},
        {"btn_connect", {"Connect", "اتصال"}},
        {"btn_disconnect", {"Disconnect", "قطع اتصال"}},
        {"btn_attach", {"Attach", "اتصال دستگاه"}},
        {"btn_attach_ro", {"Attach read-only", "اتصال فقط‌خواندنی"}},
        {"btn_detach", {"Detach", "جداسازی"}},
        {"btn_reset", {"Reset", "ریست"}},
        {"btn_diag", {"Diagnostics", "عیب‌یابی"}},
        {"state_disconnected", {"Disconnected", "قطع"}},
        {"state_connecting", {"Connecting", "در حال اتصال"}},
        {"state_authenticating", {"Authenticating", "در حال احراز هویت"}},
        {"state_connected", {"Connected", "متصل"}},
        {"state_reconnecting", {"Reconnecting", "اتصال مجدد"}},
        {"state_failed", {"Failed", "ناموفق"}},
        {"status_attached", {"Attached to this PC", "متصل به این رایانه"}},
        {"status_available", {"Available", "در دسترس"}},
        {"status_shared", {"Shared", "به‌اشتراک‌گذاشته"}},
        {"status_in_use", {"In use elsewhere", "در استفاده در جای دیگر"}},
        {"status_unavailable", {"Unavailable", "غیرقابل دسترس"}},
        {"access_exclusive", {"Exclusive", "انحصاری"}},
        {"access_shared", {"Read-only", "فقط‌خواندنی"}},
        {"yes", {"Yes", "بله"}},
        {"no", {"No", "خیر"}},
        {"attached_ok", {"Attached: ", "متصل شد: "}},
        {"detached_ok", {"Detached.", "جدا شد."}},
        {"no_server", {"Select a server first.", "ابتدا یک سرور انتخاب کنید."}},
        {"no_device", {"Select a device first.", "ابتدا یک دستگاه انتخاب کنید."}},
        {"no_session", {"This device is not attached to this PC.", "این دستگاه به این رایانه متصل نیست."}},
        {"confirm_detach", {"Detach this device from this PC?", "این دستگاه از این رایانه جدا شود؟"}},
        {"confirm_remove", {"Remove this server from the list?", "این سرور از فهرست حذف شود؟"}},
        {"error_title", {"Operation failed", "عملیات ناموفق بود"}},
        {"diag_title", {"Diagnostics", "عیب‌یابی"}},
        {"dlg_add_title", {"Add server", "افزودن سرور"}},
        {"lbl_name", {"Name", "نام"}},
        {"lbl_host", {"Host or IP", "میزبان یا IP"}},
        {"lbl_port", {"Port", "پورت"}},
        {"lbl_tls", {"Encrypted (TLS 1.3)", "رمزنگاری (TLS 1.3)"}},
        {"lbl_fingerprint", {"Certificate fingerprint (optional)", "اثرانگشت گواهی (اختیاری)"}},
        {"lbl_token", {"Token", "توکن"}},
        {"lbl_auto", {"Auto-attach devices matching (optional)", "اتصال خودکار دستگاه‌های مطابق (اختیاری)"}},
        {"name_exists", {"A server with this name already exists.", "سروری با این نام وجود دارد."}},
        {"name_required", {"Name and host are required.", "نام و میزبان الزامی است."}},
        {"language", {"Language", "زبان"}},
        {"theme", {"Theme", "پوسته"}},
        {"theme_dark", {"Dark", "تیره"}},
        {"theme_light", {"Light", "روشن"}},
        {"ok", {"OK", "تأیید"}},
        {"cancel", {"Cancel", "انصراف"}},
    };
    return t;
}

}  // namespace

QString ctl(Lang lang, const char* key) {
    const auto& t = table();
    const auto it = t.find(key);
    if (it == t.end()) return QString::fromUtf8(key);
    return QString::fromUtf8(lang == Lang::Fa ? it->second.fa : it->second.en);
}

}  // namespace pf::gui
