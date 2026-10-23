// Restore per-record valid times after changing a reused GRIB template's step.
#pragma once
#include <eccodes.h>
#include <string>
#include <stdexcept>
namespace avenc_grib_time {
inline void restore(codes_handle* h, const std::string& timestamp) {
    if (!h || timestamp.size() < 16 || timestamp[4] != '-' ||
        timestamp[7] != '-' || timestamp[10] != 'T' || timestamp[13] != ':') return;
    for (int i : {0,1,2,3,5,6,8,9,11,12,14,15})
        if (timestamp[i] < '0' || timestamp[i] > '9') return;
    const long year=std::stol(timestamp.substr(0,4)), month=std::stol(timestamp.substr(5,2));
    const long day=std::stol(timestamp.substr(8,2)), hour=std::stol(timestamp.substr(11,2));
    const long minute=std::stol(timestamp.substr(14,2));
    if (codes_is_defined(h, "yearOfEndOfOverallTimeInterval")) {
        // validityDate/Time are read-only computed keys. Statistical GRIB2
        // products store their valid time in explicit end-of-interval keys.
        const char* keys[]={"yearOfEndOfOverallTimeInterval", "monthOfEndOfOverallTimeInterval",
            "dayOfEndOfOverallTimeInterval", "hourOfEndOfOverallTimeInterval",
            "minuteOfEndOfOverallTimeInterval", "secondOfEndOfOverallTimeInterval"};
        const long values[]={year,month,day,hour,minute,
            timestamp.size() >= 19 ? std::stol(timestamp.substr(17,2)) : 0};
        for (int i=0;i<6;++i)
            if (codes_set_long(h,keys[i],values[i]))
                throw std::runtime_error(std::string("Cannot restore GRIB valid time: ")+keys[i]);
        long date=0,time=0;
        if (codes_get_long(h,"validityDate",&date) || codes_get_long(h,"validityTime",&time) ||
            date!=year*10000+month*100+day || time!=hour*100+minute)
            throw std::runtime_error("Restored statistical GRIB valid time disagrees with AVENC metadata");
    } else {
        // Retain the existing behavior for non-statistical product templates.
        (void)codes_set_long(h,"validityDate",year*10000+month*100+day);
        (void)codes_set_long(h,"validityTime",hour*100+minute);
    }
}
} // namespace avenc_grib_time
