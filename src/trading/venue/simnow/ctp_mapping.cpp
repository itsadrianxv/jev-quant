#include "ctp_mapping.h"
#include <cmath>

namespace Trading {
namespace Ctp {
Common::Price price(double value) {
    if(!std::isfinite(value) || value<=0 || value>=static_cast<double>(Common::Price_INVALID)/100000000.0)
        return Common::Price_INVALID;
    return static_cast<Common::Price>(std::llround(value*100000000.0));
}
double decimal(Common::Price value) { return static_cast<double>(value)/100000000.0; }
}
}  // namespace Trading
