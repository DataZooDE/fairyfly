#include "include/server_clock.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace fairyfly::sap {

std::string format_utc_offset(int utc_offset_minutes) {
    const char sign = utc_offset_minutes < 0 ? '-' : '+';
    const int absolute = std::abs(utc_offset_minutes);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%c%02d:%02d", sign, absolute / 60, absolute % 60);
    return buffer;
}

json server_time_fields_at(std::chrono::system_clock::time_point now, int utc_offset_minutes) {
    const std::time_t shifted = std::chrono::system_clock::to_time_t(now) + utc_offset_minutes * 60;
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &shifted);
#else
    gmtime_r(&shifted, &parts);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &parts);

    json fields;
    fields["server_time"] = nullptr;
    fields["server_time_source"] = "unavailable";
    fields["server_time_note"] =
        "SAP GUI Scripting exposes no server clock (GuiSessionInfo has no date, time or time zone) and reading it "
        "would need a screen change. client_time is the PC clock, not the SAP server's: it equals the server time "
        "only if the PC and the SAP instance use the same time zone. For an exact server time open System > Status "
        "(or a transaction that shows it) in the session.";
    fields["client_time"] = std::string(buffer) + format_utc_offset(utc_offset_minutes);
    fields["client_utc_offset"] = format_utc_offset(utc_offset_minutes);
    return fields;
}

json server_time_fields() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
    const std::time_t as_utc = _mkgmtime(&local);
#else
    localtime_r(&t, &local);
    const std::time_t as_utc = timegm(&local);
#endif
    const int offset_minutes = static_cast<int>((as_utc - t) / 60);
    return server_time_fields_at(now, offset_minutes);
}

std::string server_time_summary(const json& fields) {
    if (!fields.is_object()) return {};
    if (fields.contains("server_time") && fields["server_time"].is_string()) {
        std::string line = "SAP server time: " + fields["server_time"].get<std::string>();
        if (fields.contains("server_time_zone") && fields["server_time_zone"].is_string())
            line += " (" + fields["server_time_zone"].get<std::string>() + ")";
        return line;
    }
    std::string line = "SAP server time: unavailable through the SAP GUI Scripting API";
    if (fields.contains("client_time") && fields["client_time"].is_string())
        line += "; client PC time is " + fields["client_time"].get<std::string>() +
                " (equals the server time only if both use the same time zone)";
    return line;
}

} // namespace fairyfly::sap
