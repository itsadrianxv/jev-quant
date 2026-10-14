#include "simnow_market_data.h"
#include "ctp_mapping.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <syncstream>

namespace Trading {
using Ctp::stage;
SimNowMarketData::SimNowMarketData(MarketDataConsumer* m, const SimNowConfig& c,
        Ctp::Transport* t, SimNowControl& shared)
    : market(m), config(c), transport(t), control(shared), market_events(c.capacity) {}

void SimNowMarketData::enqueue(Ctp::Event event) noexcept {
    // One SDK producer and one business-worker consumer per bounded SPSC queue.
    if (auto* slot = market_events.tryGetNextToWriteTo()) {
        *slot = std::move(event);
        market_events.updateWriteIndex();
        return;
    }
    control.overflow = true;
}

void SimNowMarketData::depth(const CThostFtdcDepthMarketDataField& p, const Common::PerformanceTrace& trace) {
    if(config.instrument!=p.InstrumentID) return;
    {
        std::scoped_lock lock(control.market_state_mutex);
        control.market_state.market_day=p.TradingDay;
        if(!control.market_state.trading_day.empty() && p.TradingDay[0] && control.market_state.trading_day!=p.TradingDay) {
            // TODO(simnow-rollover): Reconcile and roll positions before continuing across trading days.
            throw std::runtime_error("Trading day changed; restart from an empty account");
        }
    }
    Exchange::MarketUpdate u; u.type_=Exchange::MarketUpdateType::DEPTH_SNAPSHOT; u.ticker_id_=config.ticker_id;
    u.trace_ = trace;
    auto& d=u.depth_snapshot_;
    d.last_price_=Ctp::price(p.LastPrice); d.upper_limit_price_=Ctp::price(p.UpperLimitPrice);
    d.lower_limit_price_=Ctp::price(p.LowerLimitPrice);
    d.volume_=p.Volume>=0 ? static_cast<Common::Qty>(p.Volume) : Common::Qty_INVALID;
    d.open_interest_=std::isfinite(p.OpenInterest) && p.OpenInterest>=0 && p.OpenInterest<1e100 ? p.OpenInterest : std::numeric_limits<double>::quiet_NaN();
    const double bids[]={p.BidPrice1,p.BidPrice2,p.BidPrice3,p.BidPrice4,p.BidPrice5};
    const double asks[]={p.AskPrice1,p.AskPrice2,p.AskPrice3,p.AskPrice4,p.AskPrice5};
    const int bq[]={p.BidVolume1,p.BidVolume2,p.BidVolume3,p.BidVolume4,p.BidVolume5};
    const int aq[]={p.AskVolume1,p.AskVolume2,p.AskVolume3,p.AskVolume4,p.AskVolume5};
    for(std::size_t i=0;i<5;++i) {
        if(bq[i]>0 && Ctp::price(bids[i])!=Common::Price_INVALID) d.bids_[i]={Ctp::price(bids[i]),static_cast<Common::Qty>(bq[i])};
        if(aq[i]>0 && Ctp::price(asks[i])!=Common::Price_INVALID) d.asks_[i]={Ctp::price(asks[i]),static_cast<Common::Qty>(aq[i])};
    }
    market->publishMarketUpdate(u);
    {
        std::scoped_lock lock(control.market_state_mutex);
        control.market_state.quote_valid=d.bids_[0].price_!=Common::Price_INVALID &&
            d.asks_[0].price_!=Common::Price_INVALID && d.asks_[0].price_>d.bids_[0].price_;
    }
}

void SimNowMarketData::marketRequest(Kind kind) {
    const auto id=++market_next_id;
    if(kind==Kind::MarketLogin) market_login_request=id;
    std::osyncstream(std::clog)<<"event=simnow_startup stage="<<stage(kind)<<" request="<<id<<'\n';
    if(transport->request(kind,id)!=0) throw std::runtime_error("CTP startup request not submitted");
}

void SimNowMarketData::marketEvent(const Ctp::Event& e) {
    if(e.kind==Kind::Disconnected) {
        control.disconnected=true;
        throw std::runtime_error("CTP disconnected; execution status may be unknown");
    }
    if(control.fatal || control.stopping) return;
    if(e.kind==Kind::Depth) {
        if(auto p=std::get_if<CThostFtdcDepthMarketDataField>(&e.payload)) depth(*p, e.trace);
        return;
    }
    if(e.error) throw std::runtime_error("CTP response error code="+std::to_string(e.error));
    if(e.kind==Kind::MarketConnected) {
        if(market_connected) throw std::runtime_error("CTP reconnection is unsupported");
        market_connected=true;
        marketRequest(Kind::MarketLogin);
    } else if(e.kind==Kind::MarketLogin) {
        // SimNow may return zero instead of echoing the sole pending MD login ID.
        if(!market_login_request || !e.last ||
           (e.request_id!=0 && e.request_id!=market_login_request)) return;
        market_login_request=0;
        marketRequest(Kind::Subscription);
    } else if(e.kind==Kind::Subscription) {
        std::scoped_lock lock(control.market_state_mutex);
        control.market_state.subscribed=e.last;
    }
}

void SimNowMarketData::pollMarket() noexcept {
    try {
        for(std::size_t batch=0; batch<256 && !control.stopping && !control.fatal; ++batch) {
            const auto* pending=market_events.getNextToRead();
            if(!pending) break;
            const auto received_ns = pending->trace.source_id ? Common::nowSteadyNs() : 0;
            auto observed = *pending;
            if (observed.trace.source_id) observed.trace.t[1] = received_ns;
            try { marketEvent(observed); } catch(const std::exception& ex) { control.signalFailure(ex.what()); }
            market_events.updateReadIndex();
        }
    } catch(const std::exception& ex) { control.signalFailure(ex.what()); }
}

}  // namespace Trading
