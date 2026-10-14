#pragma once
#include <functional>
#include <memory>
#include "ctp_event.h"
#include "simnow_config.h"
#include "common/time_utils.h"

namespace Trading::Ctp {
// Internal SDK seam: fake transports drive the same event queues in offline tests.
class Transport {
  public:
    using Sink = std::function<void(Event)>;
    virtual ~Transport() = default;
    // One sink per CTP producer thread: the trader API and the market API each
    // deliver every callback from their own single internal thread. The
    // adapter backs each sink with its own SPSC queue, so a sink must never be
    // invoked from more than one thread and no event may cross sinks.
    virtual void start(const SimNowConfig&, Sink trader, Sink market) = 0;
    virtual void stop() noexcept = 0;
    virtual int request(Kind, int) = 0;
    virtual int insert(const CThostFtdcInputOrderField&, int) = 0;
    virtual int cancel(const CThostFtdcInputOrderActionField&, int) = 0;
    // Test transports use this default boundary. Native overrides stamp directly
    // before the SDK call, after copying the SDK argument.
    virtual int insertObserved(const CThostFtdcInputOrderField& p, int id, Common::PerformanceTrace& trace) {
        if (trace.t[4]) trace.t[7] = Common::nowSteadyNs();
        return insert(p, id);
    }
    virtual int cancelObserved(const CThostFtdcInputOrderActionField& p, int id, Common::PerformanceTrace& trace) {
        if (trace.t[4]) trace.t[7] = Common::nowSteadyNs();
        return cancel(p, id);
    }
};
std::unique_ptr<Transport> makeTransport();
Common::Price price(double);
double decimal(Common::Price);
}
