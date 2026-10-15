#include "simnow_execution.h"
#include "ctp_mapping.h"
#include "trading/performance/measurement.h"
#include <algorithm>
#include <iostream>
#include <syncstream>

namespace Trading {
namespace {
using Ctp::put;
using Ctp::stage;
bool terminal(char status) {
    return status==THOST_FTDC_OST_AllTraded || status==THOST_FTDC_OST_Canceled ||
           status==THOST_FTDC_OST_PartTradedNotQueueing || status==THOST_FTDC_OST_NoTradeNotQueueing;
}
}
SimNowExecution::SimNowExecution(OrderGateway* g, const SimNowConfig& c,
        Ctp::Transport* t, SimNowControl& shared, StateSink sink)
    : gateway(g), config(c), transport(t), control(shared), state_sink(std::move(sink)),
      trader_events(c.capacity) {}

void SimNowExecution::handleRequest(const Exchange::ClientRequest& request) noexcept {
    try { submit(request); }
    catch (const std::exception& ex) { fail(ex.what()); }
}

void SimNowExecution::enqueue(Ctp::Event event) noexcept {
    // One SDK producer and one business-worker consumer per bounded SPSC queue.
    if (auto* slot = trader_events.tryGetNextToWriteTo()) {
        *slot = std::move(event);
        trader_events.updateWriteIndex();
        return;
    }
    control.overflow = true;
}

void SimNowExecution::state(bool enabled) {
    if(state_published && ready==enabled && account.available_margin_==published_account.available_margin_ &&
       account.used_margin_==published_account.used_margin_ && account.equity_==published_account.equity_) return;
    ready=enabled;
    published_account=account; state_published=true;
    state_sink(enabled,account,instrument.VolumeMultiple>0 ? instrument.VolumeMultiple : 1);
}

void SimNowExecution::fail(const std::string& reason) {
    control.signalFailure(reason);
    state(false);
}

void SimNowExecution::request(Kind kind) {
    const auto id=++next_id;
    requests[id]=kind;
    std::osyncstream(std::clog)<<"event=simnow_startup stage="<<stage(kind)<<" request="<<id<<'\n';
    if(transport->request(kind,id)!=0) throw std::runtime_error("CTP startup request not submitted");
}

void SimNowExecution::publish(Order& o,Exchange::ClientResponseType type,int qty,Common::Price price) {
    Exchange::ClientResponse r;
    r.type_=type; r.client_id_=gateway->clientId(); r.ticker_id_=config.ticker_id;
    r.client_order_id_=o.request.order_id_; r.venue_order_id_=o.request.order_id_;
    r.side_=o.request.side_; r.offset_=o.request.offset_; r.order_type_=Common::OrderType::LIMIT;
    r.price_=price==Common::Price_INVALID ? o.request.price_ : price;
    r.exec_qty_=qty; r.leaves_qty_=o.request.qty_-o.fills;
    gateway->publishClientResponse(gateway->nextExpectedResponseSequence(),r);
    std::osyncstream(std::clog)<<"event=simnow_response order="<<r.client_order_id_
        <<" type="<<Exchange::clientResponseTypeToString(type)<<" exec_qty="<<qty<<'\n';
}

bool SimNowExecution::outstanding() const {
    return std::any_of(orders.begin(),orders.end(),[](const auto& entry){return !entry.second.done;});
}

void SimNowExecution::finish(Order& o) {
    if(o.done || !o.has_terminal || o.fills<o.reported_fills) return;
    if(o.fills>o.reported_fills) throw std::runtime_error("CTP order/trade quantity mismatch");
    if(o.fills<static_cast<int>(o.request.qty_))
        publish(o,o.rejected ? Exchange::ClientResponseType::REJECTED : Exchange::ClientResponseType::CANCELED);
    o.done=true;
}

void SimNowExecution::cancel(Order& o, const Exchange::ClientRequest* observed) {
    if(o.done || o.has_terminal || o.cancel_sent || control.disconnected) return;
    CThostFtdcInputOrderActionField p{};
    put(p.BrokerID,config.broker); put(p.InvestorID,config.user); put(p.UserID,config.user);
    put(p.InstrumentID,config.instrument); put(p.ExchangeID,"SHFE");
    put(p.OrderRef,o.ref); put(p.OrderSysID,o.sys);
    p.FrontID=front; p.SessionID=session; p.ActionFlag=THOST_FTDC_AF_Delete;
    p.OrderActionRef=++next_id; o.cancel_id=next_id; o.cancel_sent=true;
    auto request = observed ? *observed : Exchange::ClientRequest{};
    const auto rc=transport->cancelObserved(p,next_id,request.trace_);
    if (config.measurement && observed) config.measurement->record(request,rc);
    if(rc!=0) { publish(o,Exchange::ClientResponseType::CANCEL_REJECTED); fail("Cancellation not submitted; order remains unresolved"); }
}

void SimNowExecution::submit(const Exchange::ClientRequest& r) {
    if(r.client_id_!=gateway->clientId() || r.ticker_id_!=config.ticker_id)
        throw std::runtime_error("SimNow request identity mismatch");
    if(r.type_==Exchange::ClientRequestType::CANCEL) {
        for(auto& [ref,o]:orders) if(o.request.order_id_==r.order_id_) { cancel(o, &r); return; }
    }
    Order rejected; rejected.request=r;
    auto reject=[&]{publish(rejected,r.type_==Exchange::ClientRequestType::CANCEL ?
        Exchange::ClientResponseType::CANCEL_REJECTED : Exchange::ClientResponseType::REJECTED);};
    if(r.type_!=Exchange::ClientRequestType::NEW || !ready || control.stopping || control.fatal ||
       r.ticker_id_!=config.ticker_id || r.client_id_!=gateway->clientId() ||
       r.order_type_!=Common::OrderType::LIMIT || r.qty_!=1 || outstanding() ||
       (r.side_!=Common::Side::BUY && r.side_!=Common::Side::SELL)) { reject(); return; }
    const bool open=r.offset_==Common::OrderOffset::OPEN;
    const int side=Common::sideToValue(r.side_);
    if((open && net_position!=0) || (!open && (r.offset_!=Common::OrderOffset::CLOSE_TODAY || net_position.load()!=-side))) { reject(); return; }
    const auto tick=Ctp::price(instrument.PriceTick);
    if(r.price_<=0 || r.price_==Common::Price_INVALID || tick==Common::Price_INVALID || r.price_%tick!=0) { reject(); return; }
    CThostFtdcInputOrderField p{};
    put(p.BrokerID,config.broker); put(p.InvestorID,config.user); put(p.UserID,config.user);
    put(p.InstrumentID,config.instrument); put(p.ExchangeID,"SHFE");
    const auto ref=std::to_string(++next_ref); put(p.OrderRef,ref);
    p.Direction=side>0 ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
    p.CombOffsetFlag[0]=open ? THOST_FTDC_OF_Open : THOST_FTDC_OF_CloseToday;
    p.CombHedgeFlag[0]=THOST_FTDC_HF_Speculation;
    p.OrderPriceType=THOST_FTDC_OPT_LimitPrice; p.LimitPrice=Ctp::decimal(r.price_);
    p.VolumeTotalOriginal=1; p.TimeCondition=THOST_FTDC_TC_GFD;
    p.VolumeCondition=THOST_FTDC_VC_AV; p.MinVolume=1;
    p.ContingentCondition=THOST_FTDC_CC_Immediately; p.ForceCloseReason=THOST_FTDC_FCC_NotForceClose;
    Order o; o.request=r; o.ref=ref; o.insert_id=++next_id;
    auto& stored=orders.emplace(ref,std::move(o)).first->second;
    auto observed = r;
    const auto rc=transport->insertObserved(p,next_id,observed.trace_);
    if (config.measurement) config.measurement->record(observed,rc);
    if(rc!=0) { publish(stored,Exchange::ClientResponseType::REJECTED); stored.done=true; }
}

void SimNowExecution::orderEvent(const CThostFtdcOrderField& p) {
    const auto it=orders.find(p.OrderRef);
    if(it==orders.end() || p.FrontID!=front || p.SessionID!=session || config.instrument!=p.InstrumentID)
        throw std::runtime_error("Unexpected order on exclusive SimNow account");
    auto& o=it->second;
    if(p.OrderSysID[0]) o.sys=p.OrderSysID;
    if(p.VolumeTraded<0 || p.VolumeTraded>1) throw std::runtime_error("Invalid CTP cumulative execution quantity");
    o.reported_fills=std::max(o.reported_fills,p.VolumeTraded);
    if(o.done) return;
    if(p.OrderSubmitStatus==THOST_FTDC_OSS_CancelRejected && !o.cancel_rejected) {
        publish(o,Exchange::ClientResponseType::CANCEL_REJECTED); o.cancel_rejected=true;
    }
    if(terminal(p.OrderStatus) || p.OrderSubmitStatus==THOST_FTDC_OSS_InsertRejected) {
        o.has_terminal=true; o.rejected=p.OrderSubmitStatus==THOST_FTDC_OSS_InsertRejected;
        finish(o); return;
    }
    if(!o.accepted && (p.OrderStatus==THOST_FTDC_OST_NoTradeQueueing || p.OrderStatus==THOST_FTDC_OST_PartTradedQueueing)) {
        publish(o,Exchange::ClientResponseType::ACCEPTED); o.accepted=true;
    }
}

void SimNowExecution::tradeEvent(const CThostFtdcTradeField& p) {
    const auto it=orders.find(p.OrderRef);
    if(it==orders.end() || config.instrument!=p.InstrumentID || std::string(p.ExchangeID)!="SHFE" || trading_day!=p.TradingDay)
        throw std::runtime_error("Unexpected trade on exclusive SimNow account");
    auto& o=it->second;
    if(!o.sys.empty() && o.sys!=p.OrderSysID) throw std::runtime_error("CTP trade order identity mismatch");
    const auto key=std::string(p.TradingDay)+":"+p.ExchangeID+":"+p.TradeID;
    if(o.trades.contains(key)) return;
    if(!p.TradeID[0] || p.Volume!=1 || o.fills!=0 || o.done || Ctp::price(p.Price)==Common::Price_INVALID ||
       p.Direction!=(o.request.side_==Common::Side::BUY ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell) ||
       p.OffsetFlag!=(o.request.offset_==Common::OrderOffset::OPEN ? THOST_FTDC_OF_Open : THOST_FTDC_OF_CloseToday))
        throw std::runtime_error("Invalid CTP execution");
    o.trades.insert(key); o.fills+=p.Volume;
    net_position.fetch_add(Common::sideToValue(o.request.side_)*p.Volume);
    publish(o,Exchange::ClientResponseType::FILLED,p.Volume,Ctp::price(p.Price));
    // A trade is sufficient to close a fully filled order; late status callbacks are informational.
    o.done=true;
}

void SimNowExecution::event(const Ctp::Event& e) {
    if(e.kind==Kind::Disconnected) {
        control.disconnected=true;
        // TODO(simnow-recovery): Reconcile private flow, orders, trades, and positions before resuming.
        throw std::runtime_error("CTP disconnected; execution status may be unknown");
    }
    if(e.kind==Kind::Order) { if(auto p=std::get_if<CThostFtdcOrderField>(&e.payload)) orderEvent(*p); return; }
    if(e.kind==Kind::Trade) { if(auto p=std::get_if<CThostFtdcTradeField>(&e.payload)) tradeEvent(*p); return; }
    if(e.kind==Kind::InsertError || e.kind==Kind::CancelError) {
        if(!e.error) return;
        std::string ref;
        if(auto p=std::get_if<CThostFtdcInputOrderField>(&e.payload)) ref=p->OrderRef;
        if(auto p=std::get_if<CThostFtdcInputOrderActionField>(&e.payload)) ref=p->OrderRef;
        auto it=orders.find(ref);
        if(it==orders.end()) it=std::find_if(orders.begin(),orders.end(),[&](auto& v){ return e.request_id!=0 &&
            (e.kind==Kind::InsertError ? v.second.insert_id : v.second.cancel_id)==e.request_id; });
        if(it==orders.end()) throw std::runtime_error("Unmatched CTP order error");
        auto& o=it->second;
        if(o.done) return;
        if(e.kind==Kind::InsertError) { o.rejected=true; o.has_terminal=true; finish(o); }
        else if(!o.cancel_rejected) { publish(o,Exchange::ClientResponseType::CANCEL_REJECTED); o.cancel_rejected=true; }
        return;
    }
    if(control.fatal) return;
    if(e.error) {
        if(startup_done && query_id && e.request_id==query_id && query_kind==Kind::Account) {
            query_id=0; account={NAN,NAN,NAN}; state(false); return;
        }
        throw std::runtime_error("CTP response error code="+std::to_string(e.error));
    }
    if(e.kind==Kind::TraderConnected) {
        if(trader_connected) throw std::runtime_error("CTP reconnection is unsupported");
        trader_connected=true; request(Kind::Auth); return;
    }
    if(e.kind==Kind::Auth || e.kind==Kind::Login || e.kind==Kind::Settlement) {
        auto found=requests.find(e.request_id);
        if(found==requests.end() || found->second!=e.kind || !e.last) return;
        requests.erase(found);
        if(e.kind==Kind::Auth) request(Kind::Login);
        else if(e.kind==Kind::Login) {
            const auto* p=std::get_if<CThostFtdcRspUserLoginField>(&e.payload);
            if(!p || !p->TradingDay[0]) throw std::runtime_error("Missing CTP login session");
            front=p->FrontID; session=p->SessionID; trading_day=p->TradingDay;
            {
                std::scoped_lock lock(control.market_state_mutex);
                control.market_state.trading_day=trading_day;
                if(!control.market_state.market_day.empty() && control.market_state.market_day!=trading_day)
                    throw std::runtime_error("Trading and market data trading days disagree");
            }
            next_ref=p->MaxOrderRef[0] ? std::stoull(p->MaxOrderRef) : 0;
            request(Kind::Settlement);
        } else queries={Kind::Instrument,Kind::Account,Kind::Position,Kind::OrderQuery};
        return;
    }
    if(e.request_id!=query_id || e.kind!=query_kind || !query_id) return;
    if(auto p=std::get_if<CThostFtdcInstrumentField>(&e.payload);
       p && config.instrument==p->InstrumentID) {
        if(std::string(p->ExchangeID)!="SHFE" ||
           p->ProductClass!=THOST_FTDC_PC_Futures || !p->IsTrading || p->VolumeMultiple<=0 ||
           Ctp::price(p->PriceTick)==Common::Price_INVALID || p->MinLimitOrderVolume>1 || p->MaxLimitOrderVolume<1)
            throw std::runtime_error("Configured instrument is not an eligible SHFE futures contract");
        instrument=*p; instrument_seen=true;
    }
    if(auto p=std::get_if<CThostFtdcTradingAccountField>(&e.payload)) {
        if(std::string(p->CurrencyID)!="CNY" || !std::isfinite(p->Available) || !std::isfinite(p->CurrMargin) || !std::isfinite(p->Balance))
            throw std::runtime_error("Invalid CTP account snapshot");
        account={p->Available,p->CurrMargin,p->Balance}; account_seen=true;
    }
    if(auto p=std::get_if<CThostFtdcInvestorPositionField>(&e.payload)) {
        if(p->Position!=0 || p->LongFrozen!=0 || p->ShortFrozen!=0)
            throw std::runtime_error("SimNow startup requires an empty account");
    }
    if(auto p=std::get_if<CThostFtdcOrderField>(&e.payload)) {
        if(!terminal(p->OrderStatus)) throw std::runtime_error("SimNow startup found an active order");
    }
    if(e.last) {
        if((query_kind==Kind::Instrument && !instrument_seen) || (query_kind==Kind::Account && !account_seen))
            throw std::runtime_error("Required CTP query returned no data");
        if(query_kind==Kind::OrderQuery) startup_done=true;
        query_id=0;
    }
}

void SimNowExecution::pollOrders() noexcept {
    try {
        if(control.overflow.exchange(false)) fail("CTP callback queue exhausted; execution status may be unknown");
        for(std::size_t batch=0; batch<256; ++batch) {
            const auto* pending=trader_events.getNextToRead();
            if(!pending) break;
            try { event(*pending); } catch(const std::exception& ex) { fail(ex.what()); }
            trader_events.updateReadIndex();
        }
        const auto now=Clock::now();
        if(control.stopping) {
            state(false);
            for(auto& [ref,o]:orders) cancel(o);
            if(!outstanding()) { std::scoped_lock lock(control.mutex); control.stop_done=true; control.changed.notify_all(); }
            return;
        }
        if(control.fatal) { state(false); return; }
        if(query_id && now>query_deadline) throw std::runtime_error("CTP query timed out");
        if(startup_done && now>=next_account && !query_id && queries.empty()) {
            queries.push_back(Kind::Account); next_account=now+std::chrono::seconds(30);
        }
        if(!query_id && !queries.empty() && now>=next_query) {
            query_kind=queries.front(); queries.pop_front(); query_id=++next_id;
            std::osyncstream(std::clog)<<"event=simnow_query stage="<<stage(query_kind)<<" request="<<query_id<<'\n';
            if(query_kind==Kind::Account) account_seen=false;
            const auto rc=transport->request(query_kind,query_id);
            next_query=now+config.query_interval; query_deadline=now+std::chrono::seconds(10);
            if(rc==-2 || rc==-3) {
                queries.push_front(query_kind); query_id=0;
                if(startup_done) account={NAN,NAN,NAN};
            }
            else if(rc!=0) throw std::runtime_error("CTP query not submitted");
        }
        bool market_ready;
        {
            std::scoped_lock lock(control.market_state_mutex);
            market_ready=control.market_state.subscribed && control.market_state.quote_valid;
        }
        const bool usable=!control.fatal && startup_done && market_ready && std::isfinite(account.equity_);
        state(usable);
        if(usable) {
            { std::scoped_lock lock(control.mutex); if(!control.fatal) control.active=true; }
            control.changed.notify_all();
        }
    } catch(const std::exception& ex) { fail(ex.what()); }
}

}  // namespace Trading
