#include "simnow_venue_adapter.h"
#include "simnow_market_data.h"
#include "simnow_execution.h"
#include <iostream>
#include <syncstream>

namespace Trading {
struct SimNowVenueAdapter::Impl {
    MarketDataConsumer* market;
    OrderGateway* gateway;
    SimNowConfig config;
    std::unique_ptr<Ctp::Transport> transport;
    SimNowControl control;
    bool started = false, used = false;
    SimNowMarketData market_data;
    SimNowExecution execution;

    Impl(MarketDataConsumer* m, OrderGateway* g, SimNowConfig c, StateSink s,
         std::unique_ptr<Ctp::Transport> t)
        : market(m), gateway(g), config(std::move(c)), transport(std::move(t)),
          market_data(m, config, transport.get(), control),
          execution(g, config, transport.get(), control, s) {
        if(!market || !gateway || !transport || !s || config.instrument.empty() ||
           config.user.empty() || config.password.empty() || config.auth_code.empty() ||
           config.ticker_id>=Common::ME_MAX_TICKERS || config.capacity==0 ||
           config.startup_timeout.count()<=0 || config.shutdown_timeout.count()<=0)
            throw std::invalid_argument("Invalid SimNow configuration");
        if(!config.trading_front.starts_with("tcp://") || !config.market_front.starts_with("tcp://"))
            throw std::invalid_argument("SimNow fronts must use tcp://");
    }
};

SimNowVenueAdapter::SimNowVenueAdapter(MarketDataConsumer* m, OrderGateway* g,
        SimNowConfig c, StateSink s)
    : SimNowVenueAdapter(m, g, std::move(c), std::move(s), Ctp::makeTransport()) {}

SimNowVenueAdapter::SimNowVenueAdapter(MarketDataConsumer* m,OrderGateway* g,SimNowConfig c,StateSink s,std::unique_ptr<Ctp::Transport> t)
    : impl_(std::make_unique<Impl>(m,g,std::move(c),std::move(s),std::move(t))) {}
SimNowVenueAdapter::~SimNowVenueAdapter() { stop(); }
void SimNowVenueAdapter::prepare() {
    auto& i=*impl_;
    if(i.used) throw std::logic_error("SimNow adapter cannot be prepared twice");
    if(i.market->running() || i.gateway->running())
        throw std::logic_error("Prepare SimNow before starting business workers");
    i.started=true; i.used=true;
    i.gateway->setRequestHandler([&i](std::size_t,const auto& r){i.execution.handleRequest(r);});
    i.gateway->setPollHandler([&i]{i.execution.pollOrders();});
    i.market->setPollHandler([&i]{i.market_data.pollMarket();});
    try {
        i.transport->start(i.config,
            [&i](auto e){i.execution.enqueue(std::move(e));},
            [&i](auto e){i.market_data.enqueue(std::move(e));});
    } catch(...) { stop(); throw; }
}
void SimNowVenueAdapter::waitUntilReady() {
    auto& i=*impl_;
    if(!i.started || !i.market->running() || !i.gateway->running())
        throw std::logic_error("SimNow readiness requires prepared transport and running workers");
    std::unique_lock lock(i.control.mutex);
    const auto ok=i.control.changed.wait_for(lock,i.config.startup_timeout,[&]{return i.control.active.load() || i.control.fatal.load();});
    if(!ok || i.control.fatal) {
        lock.unlock();
        // The gateway still owns account state until the caller joins it.
        if(!ok) i.control.signalFailure("SimNow startup timed out");
        throw std::runtime_error(failure());
    }
}
void SimNowVenueAdapter::quiesce() {
    auto& i=*impl_;
    if(!i.started || i.control.stopping.exchange(true)) return;
    if(i.gateway->running()) {
        std::unique_lock lock(i.control.mutex);
        const auto ok=i.control.changed.wait_for(lock,i.config.shutdown_timeout,[&]{return i.control.stop_done;});
        lock.unlock();
        if(!ok) i.control.signalFailure("Shutdown timeout; order state unresolved");
    }
}
void SimNowVenueAdapter::stop() {
    auto& i=*impl_;
    if(i.market->running() || i.gateway->running())
        throw std::logic_error("Join business workers before stopping SimNow");
    if(!i.started) return;
    i.control.stopping=true;
    i.transport->stop();
    i.market->setPollHandler({});
    i.gateway->setPollHandler({}); i.gateway->setRequestHandler({});
    i.execution.state(false);
    i.control.active=false; i.started=false;
    std::osyncstream(std::clog)<<"event=simnow_stopped position="<<i.execution.position()<<" unresolved="<<i.execution.outstanding()<<'\n';
}
bool SimNowVenueAdapter::running() const noexcept { return impl_->control.active.load(); }
bool SimNowVenueAdapter::failed() const noexcept { return impl_->control.fatal.load(); }
std::string SimNowVenueAdapter::failure() const { std::scoped_lock lock(impl_->control.mutex); return impl_->control.error; }
int SimNowVenueAdapter::position() const noexcept { return impl_->execution.position(); }
}
