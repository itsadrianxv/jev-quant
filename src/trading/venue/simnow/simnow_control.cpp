#include "simnow_control.h"
#include <iostream>
#include <syncstream>

namespace Trading {
void SimNowControl::signalFailure(const std::string& reason) {
    {
        std::scoped_lock lock(mutex);
        if(error.empty()) error=reason;
        fatal=true; active=false;
    }
    changed.notify_all();
    std::osyncstream(std::cerr)<<"event=simnow_failure reason="<<reason<<'\n';
}
}  // namespace Trading
