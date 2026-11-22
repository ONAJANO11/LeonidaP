// Emit synthetic statistics for check_pipeline_stats_csv.py; no Vulkan device is needed.
#include "pipeline_stats_csv.h"

int main() {
    std::FILE* f = std::fopen("stats.csv", "wb");
    if (!f) return 1;
    const char* fields[][3] = {
        {"vs_plain", "vs", "Register Count"},
        {"vs_comma,variant", "ps", "Reg \"Count\""},
        {"cr\ronly", "lf\nonly", "crlf\r\nonly"},
        {"", " vs ", "bytes, \"quoted\"\r\nnext"},
    };
    for (const auto& row : fields) {
        write_stats_csv_prefix(f, row[0], row[1], row[2]);
        std::fprintf(f, "17%s", stats_csv_eol);
    }
    return std::fclose(f) != 0;
}
