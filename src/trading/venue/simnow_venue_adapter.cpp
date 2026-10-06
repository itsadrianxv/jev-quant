#include "simnow_venue_adapter.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <syncstream>

namespace Trading {
namespace {
using Clock = std::chrono::steady_clock;
using Kind = Ctp::Kind;
const char* stage(Kind kind) {
    switch(kind) {
    case Kind::Auth: return "authenticate";
    case Kind::Login: return "trader_login";
    case Kind::MarketLogin: return "market_login";
    case Kind::Settlement: return "settlement_confirmation";
    case Kind::Instrument: return "instrument_query";
    case Kind::Account: return "account_query";
    case Kind::Position: return "position_query";
    case Kind::OrderQuery: return "order_query";
    case Kind::Subscription: return "market_subscription";
    default: return "unknown";
    }
}
template<std::size_t N> void put(char (&out)[N], const std::string& value) {
    if (value.size() >= N) throw std::invalid_argument("CTP field exceeds SDK width");
    std::memcpy(out,value.c_str(),value.size()+1);
}
std::string trim(const std::string& value) {
    const auto first=value.find_first_not_of(" \t\r\n");
    if(first==std::string::npos) return {};
    return value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
}
std::string credential(const char* name,const std::string& path) {
    const auto* value=std::getenv(name);
    if(value && *value) return value;
    std::ifstream file(path);
    std::string line;
    while(std::getline(file,line)) {
        if(line.starts_with("\xEF\xBB\xBF")) line.erase(0,3);
        line=trim(line);
        if(line.empty() || line.front()=='#') continue;
        if(line.starts_with("export ")) line=trim(line.substr(7));
        const auto equals=line.find('=');
        if(equals==std::string::npos || trim(line.substr(0,equals))!=name) continue;
        auto result=trim(line.substr(equals+1));
        if(!result.empty() && (result.front()=='\'' || result.front()=='"')) {
            const auto end=result.find(result.front(),1);
            if(end==std::string::npos ||
               (!trim(result.substr(end+1)).empty() && trim(result.substr(end+1)).front()!='#'))
                throw std::runtime_error(std::string("Invalid dotenv value for ")+name);
            result=result.substr(1,end-1);
        } else {
            const auto comment=result.find(" #");
            if(comment!=std::string::npos) result=trim(result.substr(0,comment));
        }
        if(!result.empty()) return result;
        break;
    }
    throw std::runtime_error(std::string("Missing credential in environment or dotenv: ")+name);
}
bool terminal(char status) {
    return status==THOST_FTDC_OST_AllTraded || status==THOST_FTDC_OST_Canceled ||
           status==THOST_FTDC_OST_PartTradedNotQueueing || status==THOST_FTDC_OST_NoTradeNotQueueing;
}
}
namespace Ctp {
Common::Price price(double value) {
    if(!std::isfinite(value) || value<=0 || value>=static_cast<double>(Common::Price_INVALID)/100000000.0)
        return Common::Price_INVALID;
    return static_cast<Common::Price>(std::llround(value*100000000.0));
}
double decimal(Common::Price value) { return static_cast<double>(value)/100000000.0; }
}

SimNowConfig SimNowConfig::fromJson(const nlohmann::json& j,Common::TickerId ticker,
                                  const std::string& dotenv_path) {
    SimNowConfig c;
    c.trading_front=j.at("trading_front").get<std::string>();
    c.market_front=j.at("market_front").get<std::string>();
    c.instrument=j.at("instrument").get<std::string>();
    c.user=credential("SIMNOW_USER_ID",dotenv_path);
    c.password=credential("SIMNOW_PASSWORD",dotenv_path);
    c.auth_code="0000000000000000";
    c.ticker_id=ticker;
    c.flow_directory=j.value("flow_directory",std::string("flows/simnow"));
    c.startup_timeout=std::chrono::milliseconds(j.value("startup_timeout_ms",60000));
    c.shutdown_timeout=std::chrono::milliseconds(j.value("shutdown_timeout_ms",10000));
    return c;
}

struct SimNowVenueAdapter::Impl {
    MarketDataConsumer* market;
    OrderGateway* gateway;
    SimNowConfig config;
    StateSink state_sink;
    std::unique_ptr<Ctp::Transport> transport;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Ctp::Event> events;
    std::string error;
    std::atomic<bool> active{false}, fatal{false}, stopping{false}, overflow{false};
    std::atomic<int> net_position{0};
    bool started=false, used=false, market_started=false, ready=false, disconnected=false;
    bool trader_connected=false, market_connected=false, subscribed=false, quote_valid=false;
    bool instrument_seen=false, account_seen=false, startup_done=false, stop_done=false;
    int next_id=0, query_id=0, front=0, session=0;
    std::uint64_t next_ref=0;
    std::string trading_day;
    std::string market_day;
    Kind query_kind=Kind::Instrument;
    std::deque<Kind> queries;
    std::map<int,Kind> requests;
    Clock::time_point next_query{}, query_deadline{}, next_account{};
    CThostFtdcInstrumentField instrument{};
    AccountState account{};
    AccountState published_account{NAN,NAN,NAN};
    bool state_published=false;
    struct Order {
        Exchange::ClientRequest request;
        std::string ref, sys;
        int insert_id=0, cancel_id=0, fills=0, reported_fills=0;
        bool accepted=false, done=false, cancel_sent=false, cancel_rejected=false, has_terminal=false, rejected=false;
        std::set<std::string> trades;
    };
    std::map<std::string,Order> orders;

    Impl(MarketDataConsumer* m,OrderGateway* g,SimNowConfig c,StateSink s,std::unique_ptr<Ctp::Transport> t)
        : market(m),gateway(g),config(std::move(c)),state_sink(std::move(s)),transport(std::move(t)) {
        if(!market || !gateway || !transport || !state_sink || config.instrument.empty() ||
           config.user.empty() || config.password.empty() || config.auth_code.empty() ||
           config.ticker_id>=Common::ME_MAX_TICKERS || config.capacity==0 ||
           config.startup_timeout.count()<=0 || config.shutdown_timeout.count()<=0)
            throw std::invalid_argument("Invalid SimNow configuration");
        if(!config.trading_front.starts_with("tcp://") || !config.market_front.starts_with("tcp://"))
            throw std::invalid_argument("SimNow fronts must use tcp://");
    }
    void state(bool enabled) {
        if(state_published && ready==enabled && account.available_margin_==published_account.available_margin_ &&
           account.used_margin_==published_account.used_margin_ && account.equity_==published_account.equity_) return;
        ready=enabled;
        published_account=account; state_published=true;
        state_sink(enabled,account,instrument.VolumeMultiple>0 ? instrument.VolumeMultiple : 1);
    }
    void fail(const std::string& reason) {
        { std::scoped_lock lock(mutex); if(error.empty()) error=reason; }
        fatal=true; active=false; state(false); changed.notify_all();
        std::osyncstream(std::cerr)<<"event=simnow_failure reason="<<reason<<'\n';
    }
    void enqueue(Ctp::Event event) noexcept {
        try {
            std::scoped_lock lock(mutex);
            if(events.size()>=config.capacity) { overflow=true; return; }
            events.push_back(std::move(event));
        } catch(...) { overflow=true; }
    }
    void request(Kind kind) {
        const auto id=++next_id;
        requests[id]=kind;
        std::osyncstream(std::clog)<<"event=simnow_startup stage="<<stage(kind)<<" request="<<id<<'\n';
        if(transport->request(kind,id)!=0) throw std::runtime_error("CTP startup request not submitted");
    }
    void publish(Order& o,Exchange::ClientResponseType type,int qty=0,Common::Price price=Common::Price_INVALID) {
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
    bool outstanding() const {
        return std::any_of(orders.begin(),orders.end(),[](const auto& entry){return !entry.second.done;});
    }
    void finish(Order& o) {
        if(o.done || !o.has_terminal || o.fills<o.reported_fills) return;
        if(o.fills>o.reported_fills) throw std::runtime_error("CTP order/trade quantity mismatch");
        if(o.fills<static_cast<int>(o.request.qty_))
            publish(o,o.rejected ? Exchange::ClientResponseType::REJECTED : Exchange::ClientResponseType::CANCELED);
        o.done=true;
    }
    void cancel(Order& o) {
        if(o.done || o.has_terminal || o.cancel_sent || disconnected) return;
        CThostFtdcInputOrderActionField p{};
        put(p.BrokerID,config.broker); put(p.InvestorID,config.user); put(p.UserID,config.user);
        put(p.InstrumentID,config.instrument); put(p.ExchangeID,"SHFE");
        put(p.OrderRef,o.ref); put(p.OrderSysID,o.sys);
        p.FrontID=front; p.SessionID=session; p.ActionFlag=THOST_FTDC_AF_Delete;
        p.OrderActionRef=++next_id; o.cancel_id=next_id; o.cancel_sent=true;
        const auto rc=transport->cancel(p,next_id);
        if(rc!=0) { publish(o,Exchange::ClientResponseType::CANCEL_REJECTED); fail("Cancellation not submitted; order remains unresolved"); }
    }
    void submit(const Exchange::ClientRequest& r) {
        if(r.client_id_!=gateway->clientId() || r.ticker_id_!=config.ticker_id)
            throw std::runtime_error("SimNow request identity mismatch");
        if(r.type_==Exchange::ClientRequestType::CANCEL) {
            for(auto& [ref,o]:orders) if(o.request.order_id_==r.order_id_) { cancel(o); return; }
        }
        Order rejected; rejected.request=r;
        auto reject=[&]{publish(rejected,r.type_==Exchange::ClientRequestType::CANCEL ?
            Exchange::ClientResponseType::CANCEL_REJECTED : Exchange::ClientResponseType::REJECTED);};
        if(r.type_!=Exchange::ClientRequestType::NEW || !ready || stopping || fatal ||
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
        const auto rc=transport->insert(p,next_id);
        if(rc!=0) { publish(stored,Exchange::ClientResponseType::REJECTED); stored.done=true; }
    }
    void depth(const CThostFtdcDepthMarketDataField& p) {
        if(config.instrument!=p.InstrumentID) return;
        market_day=p.TradingDay;
        if(!trading_day.empty() && p.TradingDay[0] && trading_day!=p.TradingDay) {
            // TODO(simnow-rollover): Reconcile and roll positions before continuing across trading days.
            throw std::runtime_error("Trading day changed; restart from an empty account");
        }
        Exchange::MarketUpdate u; u.type_=Exchange::MarketUpdateType::DEPTH_SNAPSHOT; u.ticker_id_=config.ticker_id;
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
        quote_valid=d.bids_[0].price_!=Common::Price_INVALID && d.asks_[0].price_!=Common::Price_INVALID && d.asks_[0].price_>d.bids_[0].price_;
        market->publishMarketUpdate(u);
    }
    void orderEvent(const CThostFtdcOrderField& p) {
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
    void tradeEvent(const CThostFtdcTradeField& p) {
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
    void event(const Ctp::Event& e) {
        if(e.kind==Kind::Disconnected) {
            disconnected=true;
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
        if(fatal) return;
        if(e.kind==Kind::Depth) { if(auto p=std::get_if<CThostFtdcDepthMarketDataField>(&e.payload)) depth(*p); return; }
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
        if(e.kind==Kind::MarketConnected) {
            if(market_connected) throw std::runtime_error("CTP market reconnection is unsupported");
            market_connected=true; request(Kind::MarketLogin); return;
        }
        if(e.kind==Kind::Subscription) { subscribed=e.last; return; }
        if(e.kind==Kind::Auth || e.kind==Kind::Login || e.kind==Kind::MarketLogin || e.kind==Kind::Settlement) {
            auto found=requests.find(e.request_id);
            // SimNow's MD front returns zero instead of echoing the login request ID.
            // There is only one pending MD login; duplicates after completion stay ignored.
            if(e.kind==Kind::MarketLogin && e.request_id==0)
                found=std::find_if(requests.begin(),requests.end(),[](const auto& entry){
                    return entry.second==Kind::MarketLogin;
                });
            if(found==requests.end() || found->second!=e.kind || !e.last) return;
            requests.erase(found);
            if(e.kind==Kind::Auth) request(Kind::Login);
            else if(e.kind==Kind::MarketLogin) request(Kind::Subscription);
            else if(e.kind==Kind::Login) {
                const auto* p=std::get_if<CThostFtdcRspUserLoginField>(&e.payload);
                if(!p || !p->TradingDay[0]) throw std::runtime_error("Missing CTP login session");
                front=p->FrontID; session=p->SessionID; trading_day=p->TradingDay;
                if(!market_day.empty() && market_day!=trading_day)
                    throw std::runtime_error("Trading and market data trading days disagree");
                next_ref=p->MaxOrderRef[0] ? std::stoull(p->MaxOrderRef) : 0;
                request(Kind::Settlement);
            } else queries={Kind::Instrument,Kind::Account,Kind::Position,Kind::OrderQuery};
            return;
        }
        if(e.request_id!=query_id || e.kind!=query_kind || !query_id) return;
        if(auto p=std::get_if<CThostFtdcInstrumentField>(&e.payload)) {
            if(config.instrument!=p->InstrumentID || std::string(p->ExchangeID)!="SHFE" ||
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
    void poll() noexcept {
        try {
            if(!market_started) { market->start(); market_started=true; }
            if(overflow.exchange(false)) fail("CTP callback queue exhausted; execution status may be unknown");
            std::deque<Ctp::Event> batch;
            { std::scoped_lock lock(mutex); batch.swap(events); }
            for(const auto& e:batch) {
                try { event(e); } catch(const std::exception& ex) { fail(ex.what()); }
            }
            const auto now=Clock::now();
            if(stopping) {
                state(false);
                for(auto& [ref,o]:orders) cancel(o);
                if(!outstanding()) { std::scoped_lock lock(mutex); stop_done=true; changed.notify_all(); }
                return;
            }
            if(fatal) return;
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
            const bool usable=startup_done && subscribed && quote_valid && std::isfinite(account.equity_);
            state(usable);
            if(usable) { active=true; changed.notify_all(); }
        } catch(const std::exception& ex) { fail(ex.what()); }
    }
};

SimNowVenueAdapter::SimNowVenueAdapter(MarketDataConsumer* m,OrderGateway* g,SimNowConfig c,StateSink s,std::unique_ptr<Ctp::Transport> t)
    : impl_(std::make_unique<Impl>(m,g,std::move(c),std::move(s),std::move(t))) {}
SimNowVenueAdapter::~SimNowVenueAdapter() { stop(); }
void SimNowVenueAdapter::start() {
    auto& i=*impl_;
    if(i.used) throw std::logic_error("SimNow adapter cannot be restarted");
    i.started=true; i.used=true;
    i.gateway->setRequestHandler([&i](std::size_t,const auto& r){try {i.submit(r);} catch(const std::exception& e){i.fail(e.what());}});
    i.gateway->setPollHandler([&i]{i.poll();});
    try {
        i.transport->start(i.config,[&i](auto e){i.enqueue(std::move(e));});
        i.gateway->start();
        std::unique_lock lock(i.mutex);
        const auto ok=i.changed.wait_for(lock,i.config.startup_timeout,[&]{return i.active.load() || i.fatal.load();});
        if(!ok || i.fatal) {
            lock.unlock();
            if(!ok) { i.gateway->stop(); i.fail("SimNow startup timed out"); }
            throw std::runtime_error(failure());
        }
    } catch(...) { stop(); throw; }
}
void SimNowVenueAdapter::stop() {
    auto& i=*impl_;
    if(!i.started) return;
    i.stopping=true;
    if(i.gateway->running()) {
        std::unique_lock lock(i.mutex);
        const auto ok=i.changed.wait_for(lock,i.config.shutdown_timeout,[&]{return i.stop_done;});
        lock.unlock();
        i.gateway->stop();
        if(!ok) i.fail("Shutdown timeout; order state unresolved");
    }
    i.transport->stop(); i.market->stop();
    i.gateway->setPollHandler({}); i.gateway->setRequestHandler({});
    i.active=false; i.started=false;
    std::osyncstream(std::clog)<<"event=simnow_stopped position="<<i.net_position.load()<<" unresolved="<<i.outstanding()<<'\n';
}
bool SimNowVenueAdapter::running() const noexcept { return impl_->active.load(); }
bool SimNowVenueAdapter::failed() const noexcept { return impl_->fatal.load(); }
std::string SimNowVenueAdapter::failure() const { std::scoped_lock lock(impl_->mutex); return impl_->error; }
int SimNowVenueAdapter::position() const noexcept { return impl_->net_position.load(); }
}
