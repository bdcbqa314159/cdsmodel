#pragma once
#include <nanobind/nanobind.h>
#include <chrono>
// A caster for cdsmodel::Date (= std::chrono::sys_days) <-> datetime.date.
// Conversion goes through std::chrono::year_month_day, which is pure civil
// calendar arithmetic: no time zone, no std::tm, no mktime, no localtime.
namespace nanobind::detail {
template <> struct type_caster<std::chrono::sys_days> {
    NB_TYPE_CASTER(std::chrono::sys_days, const_name("datetime.date"))

    bool from_python(handle src, uint32_t, cleanup_list *) noexcept {
        if (!src) return false;
        try {
            std::chrono::year_month_day ymd{
                std::chrono::year{cast<int>(src.attr("year"))},
                std::chrono::month{cast<unsigned>(src.attr("month"))},
                std::chrono::day{cast<unsigned>(src.attr("day"))}};
            if (!ymd.ok()) return false;
            value = std::chrono::sys_days{ymd};
            return true;
        } catch (...) { PyErr_Clear(); return false; }
    }

    static handle from_cpp(const Value &v, rv_policy, cleanup_list *) noexcept {
        const std::chrono::year_month_day ymd{v};
        try {
            return module_::import_("datetime").attr("date")(
                       (int) ymd.year(), (unsigned) ymd.month(),
                       (unsigned) ymd.day()).release();
        } catch (...) { return handle(); }
    }
};
}
