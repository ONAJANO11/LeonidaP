// CSV cell writer for vk_pipeline_check --stats (read by Python csv with newline='').
// The file is opened in binary mode so raw CR/LF inside quoted cells survive unchanged; rows end with the native
// terminator, which keeps unquoted rows byte-identical to the former text-mode output on Windows and POSIX.
#pragma once

#include <cstdio>
#include <string_view>

#ifdef _WIN32
inline constexpr char stats_csv_eol[] = "\r\n";
#else
inline constexpr char stats_csv_eol[] = "\n";
#endif

// Writes one cell (no separator). Quotes only when the text contains a comma, double quote, CR or LF; embedded
// quotes are doubled. All other bytes (UTF-8, spaces) are written unchanged.
inline void write_stats_csv_field(std::FILE* out, std::string_view text) {
    if (text.empty()) return;
    if (text.find_first_of(",\"\r\n") == std::string_view::npos) {
        std::fwrite(text.data(), 1, text.size(), out);
        return;
    }
    std::fputc('"', out);
    for (char ch : text) {
        if (ch == '"') std::fputc('"', out);
        std::fputc(ch, out);
    }
    std::fputc('"', out);
}

inline void write_stats_csv_prefix(std::FILE* out, std::string_view name, std::string_view stage,
                                   std::string_view statistic) {
    write_stats_csv_field(out, name);
    std::fputc(',', out);
    write_stats_csv_field(out, stage);
    std::fputc(',', out);
    write_stats_csv_field(out, statistic);
    std::fputc(',', out);
}
