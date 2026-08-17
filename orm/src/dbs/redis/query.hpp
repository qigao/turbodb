#ifndef ORM_REDIS_QUERY_HPP
#define ORM_REDIS_QUERY_HPP

#include "orm_c_internal.hpp"

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace orm_c_detail {

inline std::string format_redis_double(double value)
{
    // RediSearch numeric ranges accept decimal integers and doubles but not
    // C's scientific notation (for example "1e+20"), so render every double
    // in fixed-point form with enough digits to round-trip exactly and strip
    // the redundant trailing zeros.
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(std::numeric_limits<double>::max_digits10)
           << value;
    std::string text = stream.str();
    const std::size_t dot = text.find('.');
    if (dot != std::string::npos) {
        std::size_t end = text.size();
        while (end > dot + 1 && text[end - 1] == '0')
            --end;
        if (end == dot + 1)
            --end; // no nonzero fractional digits; drop the decimal point too
        text.erase(end);
    }
    return text;
}

struct redis_query_command {
    std::vector<std::string> arguments;
    std::vector<std::string> output_columns;
    bool aggregate = false;
};

redis_query_command
build_redis_query_command(const query_plan& plan,
                          const connection_limits& limits,
                          std::string_view index_prefix);

} // namespace orm_c_detail

#endif
