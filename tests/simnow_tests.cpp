#include "trading/venue/simnow_venue_adapter.h"
#include "trading/strategy/trade_engine.h"
#include "trading/strategy/jev_http_client.h"
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace Trading;
using namespace std::chrono_literals;
using Kind=Trading::Ctp::Kind;
#define CHECK(x) do { if(!(x)) throw std::runtime_error("Check failed: " #x); } while(false)

void credentialChecks() {
    struct Environment {
        std::string key, saved;
        bool present;
        explicit Environment(const char* name):key(name),present(std::getenv(name)!=nullptr) {
            if(present) saved=std::getenv(name);
            unsetenv(name);
        }
        ~Environment() { if(present) setenv(key.c_str(),saved.c_str(),1); else unsetenv(key.c_str()); }
    } user("SIMNOW_USER_ID"), password("SIMNOW_PASSWORD");
    struct File {
        std::filesystem::path path=std::filesystem::temp_directory_path()/
            ("simnow-dotenv-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~File() {std::error_code error; std::filesystem::remove(path,error);}
    } file;
    const nlohmann::json json={{"trading_front","tcp://test:1"},{"market_front","tcp://test:2"},{"instrument","rb2610"}};
    const auto read=[&] {return SimNowConfig::fromJson(json,0,file.path.string());};
    const auto fails=[&](const std::string& expected) {
        try {read();} catch(const std::runtime_error& error) {
            CHECK(std::string(error.what()).find(expected)!=std::string::npos);
            CHECK(std::string(error.what()).find("private-value")==std::string::npos);
            return;
        }
        throw std::runtime_error("Expected credential failure");
    };
    fails("SIMNOW_USER_ID");
    { std::ofstream out(file.path); out<<"\xEF\xBB\xBF# Test fixture\r\nexport SIMNOW_USER_ID = 'file-user' # comment\r\n"
        <<"UNRELATED_KEY=ignored\r\nSIMNOW_PASSWORD=\"private-value#=$\"\r\n"; }
    auto config=read(); CHECK(config.user=="file-user"); CHECK(config.password=="private-value#=$");
    CHECK(std::getenv("SIMNOW_USER_ID")==nullptr);
    setenv("SIMNOW_USER_ID","env-user",1);
    config=read(); CHECK(config.user=="env-user"); CHECK(config.password=="private-value#=$");
    setenv("SIMNOW_PASSWORD","env-password",1);
    std::filesystem::remove(file.path);
    config=read(); CHECK(config.user=="env-user"); CHECK(config.password=="env-password");
    setenv("SIMNOW_PASSWORD","",1);
    {std::ofstream out(file.path); out<<"SIMNOW_PASSWORD=plain=value # comment\n";}
    CHECK(read().password=="plain=value");
    {std::ofstream out(file.path); out<<"SIMNOW_PASSWORD='private-value\n";}
    fails("Invalid dotenv value for SIMNOW_PASSWORD");
    {std::ofstream out(file.path); out<<"SIMNOW_PASSWORD=\n";}
    fails("SIMNOW_PASSWORD");
}

template<class F> void until(F f) {
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while(!f()) { if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error("Test wait timed out"); std::this_thread::sleep_for(1ms); }
}
template<std::size_t N> void text(char (&dest)[N],const char* src) {
    const auto length=std::strlen(src);
    CHECK(length<N);
    std::memcpy(dest,src,length+1);
}

struct Fake final : Ctp::Transport {
    Sink sink;
    std::mutex mutex;
    std::vector<CThostFtdcInputOrderField> sent;
    std::vector<CThostFtdcInputOrderActionField> canceled;
    std::atomic<int> query_count{0};
    bool dirty=false, working=false, reject_insert=false, cancel_fill=false, no_cancel=false;
    bool no_instrument=false, query_throttle=false;
    int market_login_id=-1;
    std::atomic<int> subscriptions{0};
    void start(const SimNowConfig&,Sink s) override {
        sink=std::move(s); emit(Kind::TraderConnected); emit(Kind::MarketConnected);
    }
    void stop() noexcept override {}
    void emit(Kind k,Ctp::Payload p={},int id=0,int error=0,bool last=true) {sink({k,id,error,last,std::move(p)});}
    CThostFtdcDepthMarketDataField depth() {
        CThostFtdcDepthMarketDataField p{}; text(p.InstrumentID,"au2612"); text(p.TradingDay,"20260928");
        p.BidPrice1=800; p.AskPrice1=800.02; p.BidVolume1=10; p.AskVolume1=10;
        p.LastPrice=800; p.Volume=10; return p;
    }
    int request(Kind k,int id) override {
        switch(k) {
        case Kind::Login: case Kind::MarketLogin: {
            CThostFtdcRspUserLoginField p{}; p.FrontID=7; p.SessionID=8;
            text(p.TradingDay,"20260928"); text(p.MaxOrderRef,"20");
            emit(k,p,k==Kind::MarketLogin && market_login_id>=0 ? market_login_id : id); break;
        }
        case Kind::Instrument: {
            ++query_count;
            if(query_throttle) { query_throttle=false; return -3; }
            if(no_instrument) {emit(k,{},id); break;}
            CThostFtdcInstrumentField p{}; text(p.InstrumentID,"au2612"); text(p.ExchangeID,"SHFE");
            p.PriceTick=0.02; p.VolumeMultiple=1000; p.ProductClass=THOST_FTDC_PC_Futures;
            p.IsTrading=1; p.MinLimitOrderVolume=1; p.MaxLimitOrderVolume=100; emit(k,p,id); break;
        }
        case Kind::Account: {
            CThostFtdcTradingAccountField p{}; text(p.CurrencyID,"CNY");
            p.Balance=1000000; p.Available=900000; p.CurrMargin=100000; emit(k,p,id); break;
        }
        case Kind::Position: {
            if(dirty) { CThostFtdcInvestorPositionField p{}; p.Position=1; emit(k,p,id); }
            else emit(k,{},id);
            break;
        }
        case Kind::OrderQuery: {
            if(working) {CThostFtdcOrderField p{}; p.OrderStatus=THOST_FTDC_OST_NoTradeQueueing; emit(k,p,id);}
            else emit(k,{},id);
            break;
        }
        case Kind::Subscription: ++subscriptions; emit(k,{},id); emit(Kind::Depth,depth()); break;
        default: emit(k,{},id); break;
        }
        return 0;
    }
    int insert(const CThostFtdcInputOrderField& p,int) override {
        std::scoped_lock lock(mutex); sent.push_back(p); return reject_insert ? -1 : 0;
    }
    CThostFtdcInputOrderField latest() { std::scoped_lock lock(mutex); return sent.back(); }
    std::size_t count() { std::scoped_lock lock(mutex); return sent.size(); }
    CThostFtdcOrderField order(const CThostFtdcInputOrderField& in,char status,int volume=0) {
        CThostFtdcOrderField p{}; text(p.OrderRef,in.OrderRef); text(p.InstrumentID,in.InstrumentID);
        text(p.OrderSysID,"000000001"); text(p.ExchangeID,"SHFE");
        p.FrontID=7; p.SessionID=8; p.OrderStatus=status; p.VolumeTraded=volume;
        p.OrderSubmitStatus=THOST_FTDC_OSS_Accepted; return p;
    }
    CThostFtdcTradeField trade(const CThostFtdcInputOrderField& in,const char* id="trade1") {
        CThostFtdcTradeField p{}; text(p.OrderRef,in.OrderRef); text(p.InstrumentID,in.InstrumentID);
        text(p.TradingDay,"20260928"); text(p.TradeID,id); text(p.OrderSysID,"000000001"); text(p.ExchangeID,"SHFE");
        p.Volume=1; p.Price=in.LimitPrice; p.Direction=in.Direction; p.OffsetFlag=in.CombOffsetFlag[0]; return p;
    }
    int cancel(const CThostFtdcInputOrderActionField& p,int) override {
        CThostFtdcInputOrderField in;
        {std::scoped_lock lock(mutex); canceled.push_back(p); in=sent.back();}
        if(no_cancel) return 0;
        if(cancel_fill) {
            emit(Kind::Order,order(in,THOST_FTDC_OST_AllTraded,1));
            emit(Kind::Trade,trade(in));
        } else emit(Kind::Order,order(in,THOST_FTDC_OST_Canceled));
        return 0;
    }
};

struct Fixture {
    Exchange::ClientRequestLFQueue requests{64};
    Exchange::ClientResponseLFQueue responses{64};
    Exchange::MarketUpdateLFQueue updates{128};
    MarketDataConsumer market{1,&updates};
    OrderGateway gateway{1,&requests,&responses};
    Fake* fake=nullptr;
    std::unique_ptr<SimNowVenueAdapter> adapter;
    std::atomic<bool> ready{false};
    Fixture(std::function<void(Fake&)> setup=[](Fake&){}) {
        auto transport=std::make_unique<Fake>(); fake=transport.get(); setup(*fake);
        SimNowConfig c; c.instrument="au2612"; c.user="test"; c.password="test"; c.auth_code="test";
        c.trading_front="tcp://fake:1"; c.market_front="tcp://fake:2";
        c.query_interval=1ms; c.startup_timeout=500ms; c.shutdown_timeout=30ms;
        adapter=std::make_unique<SimNowVenueAdapter>(&market,&gateway,c,[&](bool r,AccountState,double){ready=r;},std::move(transport));
    }
    void send(Common::OrderId id=1,Common::OrderOffset offset=Common::OrderOffset::OPEN,Common::Side side=Common::Side::BUY,double price=800.02) {
        Exchange::ClientRequest r; r.type_=Exchange::ClientRequestType::NEW; r.client_id_=1; r.ticker_id_=0;
        r.order_id_=id; r.offset_=offset; r.side_=side; r.order_type_=Common::OrderType::LIMIT; r.qty_=1; r.price_=Ctp::price(price);
        *requests.tryGetNextToWriteTo()=r; requests.updateWriteIndex();
    }
    Exchange::ClientResponse response() {
        until([&]{return responses.size()>0;}); auto r=*responses.getNextToRead(); responses.updateReadIndex(); return r;
    }
};

void startupChecks() {
    {
        Fixture f([](Fake& x){x.market_login_id=0;}); f.adapter->start();
        CHECK(f.ready); CHECK(f.fake->subscriptions==1);
        f.fake->emit(Kind::MarketLogin,CThostFtdcRspUserLoginField{},0);
        f.adapter->stop(); CHECK(!f.adapter->failed()); CHECK(f.fake->subscriptions==1);
    }
    {
        Fixture f([](Fake& x){x.market_login_id=999;});
        bool threw=false; try {f.adapter->start();} catch(const std::runtime_error&){threw=true;}
        CHECK(threw); CHECK(f.fake->subscriptions==0);
    }
    for(int mode=0;mode<3;++mode) {
        Fixture f([&](Fake& x){x.dirty=mode==0; x.working=mode==1; x.no_instrument=mode==2;});
        bool threw=false; try {f.adapter->start();} catch(const std::runtime_error&){threw=true;}
        CHECK(threw); CHECK(f.adapter->failed()); CHECK(!f.ready);
    }
    Fixture f([](Fake& x){x.query_throttle=true;}); f.adapter->start();
    CHECK(f.ready); CHECK(f.fake->query_count==2);
    auto snapshot=f.updates.getNextToRead()->depth_snapshot_;
    CHECK(snapshot.bids_[0].price_==Ctp::price(800)); CHECK(snapshot.bids_[1].price_==Common::Price_INVALID);
    f.adapter->stop(); CHECK(!f.adapter->failed());
}

void tradingAndDuplicates() {
    Fixture f; f.adapter->start(); f.send(); until([&]{return f.fake->count()==1;});
    auto in=f.fake->latest(); CHECK(std::string(in.OrderRef)=="21"); CHECK(in.CombOffsetFlag[0]==THOST_FTDC_OF_Open);
    CHECK(f.responses.size()==0); // SDK return zero is not acceptance.
    auto accepted=f.fake->order(in,THOST_FTDC_OST_NoTradeQueueing);
    f.fake->emit(Kind::Order,accepted); f.fake->emit(Kind::Order,accepted);
    CHECK(f.response().type_==Exchange::ClientResponseType::ACCEPTED);
    f.send(2); CHECK(f.response().type_==Exchange::ClientResponseType::REJECTED); CHECK(f.fake->count()==1);
    f.fake->emit(Kind::Order,f.fake->order(in,THOST_FTDC_OST_AllTraded,1));
    f.send(3); CHECK(f.response().type_==Exchange::ClientResponseType::REJECTED);
    auto trade=f.fake->trade(in); f.fake->emit(Kind::Trade,trade); f.fake->emit(Kind::Trade,trade);
    CHECK(f.response().exec_qty_==1); CHECK(f.adapter->position()==1);
    f.send(4); CHECK(f.response().type_==Exchange::ClientResponseType::REJECTED);
    f.send(5,Common::OrderOffset::CLOSE_TODAY,Common::Side::SELL,800);
    until([&]{return f.fake->count()==2;}); auto close=f.fake->latest();
    CHECK(close.CombOffsetFlag[0]==THOST_FTDC_OF_CloseToday);
    f.fake->emit(Kind::Trade,f.fake->trade(close,"trade2"));
    f.fake->emit(Kind::Order,f.fake->order(close,THOST_FTDC_OST_AllTraded,1));
    CHECK(f.response().type_==Exchange::ClientResponseType::FILLED);
    CHECK(f.adapter->position()==0); f.adapter->stop(); CHECK(!f.adapter->failed());
}

void shutdownAndFailures() {
    for(int mode=0;mode<3;++mode) {
        Fixture f([&](Fake& x){x.cancel_fill=mode==1; x.no_cancel=mode==2;});
        f.adapter->start(); f.send(); until([&]{return f.fake->count()==1;}); f.adapter->stop();
        CHECK(f.fake->canceled.size()==1);
        CHECK(f.adapter->failed()==(mode==2)); CHECK(f.adapter->position()==(mode==1 ? 1 : 0));
        if(mode!=2) CHECK(f.response().type_==(mode==1 ? Exchange::ClientResponseType::FILLED : Exchange::ClientResponseType::CANCELED));
    }
    Fixture f; f.adapter->start(); f.fake->emit(Kind::Disconnected,{},0,4097);
    until([&]{return f.adapter->failed();}); f.send(); CHECK(f.response().type_==Exchange::ClientResponseType::REJECTED);
    CHECK(f.fake->count()==0); f.adapter->stop();
    Fixture day; day.adapter->start(); auto d=day.fake->depth(); text(d.TradingDay,"20260929"); day.fake->emit(Kind::Depth,d);
    until([&]{return day.adapter->failed();}); day.adapter->stop();
}

void rejectionChecks() {
    Fixture local([](Fake& x){x.reject_insert=true;}); local.adapter->start(); local.send();
    CHECK(local.response().type_==Exchange::ClientResponseType::REJECTED);
    CHECK(local.adapter->position()==0); local.adapter->stop(); CHECK(!local.adapter->failed());
    Fixture f; f.adapter->start();
    f.send(1,Common::OrderOffset::OPEN,Common::Side::BUY,800.01);
    CHECK(f.response().type_==Exchange::ClientResponseType::REJECTED); CHECK(f.fake->count()==0);
    f.send(2); until([&]{return f.fake->count()==1;}); auto in=f.fake->latest();
    f.fake->emit(Kind::InsertError,in,0,31); f.fake->emit(Kind::InsertError,in,0,31);
    CHECK(f.response().type_==Exchange::ClientResponseType::REJECTED);
    f.send(3); until([&]{return f.fake->count()==2;}); in=f.fake->latest();
    auto rejected=f.fake->order(in,THOST_FTDC_OST_Canceled);
    rejected.OrderSubmitStatus=THOST_FTDC_OSS_InsertRejected;
    f.fake->emit(Kind::Order,rejected);
    CHECK(f.response().type_==Exchange::ClientResponseType::REJECTED);
    f.adapter->stop(); CHECK(!f.adapter->failed()); CHECK(f.responses.size()==0);
}

void engineContract() {
    Exchange::ClientRequestLFQueue requests(32);
    Exchange::ClientResponseLFQueue responses(32);
    Exchange::MarketUpdateLFQueue updates(32);
    JevDecisionLFQueue decisions(32);
    TradeEngine engine(1,{1,1,0},&requests,&responses,&updates,&decisions);
    engine.enableSingleOrderVenue(); engine.updateVenueState(true,{900,100,1000},1000);
    Exchange::MarketUpdate update; update.type_=Exchange::MarketUpdateType::DEPTH_SNAPSHOT; update.ticker_id_=0;
    update.depth_snapshot_.bids_[0]={Ctp::price(800),10}; update.depth_snapshot_.asks_[0]={Ctp::price(800.02),10};
    *updates.tryGetNextToWriteTo()=update; updates.updateWriteIndex(); engine.processPending();
    auto decide=[&](int id,JevBias bias,JevIntent intent){
        engine.registerEvaluation(0,id); JevDecision d; d.ticker_id_=0; d.evaluation_id_=id; d.bias_=bias; d.intent_=intent;
        *decisions.tryGetNextToWriteTo()=d; decisions.updateWriteIndex(); engine.processPending();
    };
    decide(1,JevBias::LONG,JevIntent::OPEN); CHECK(requests.size()==1);
    auto request=*requests.getNextToRead(); requests.updateReadIndex();
    Exchange::ClientResponse r; r.type_=Exchange::ClientResponseType::ACCEPTED; r.ticker_id_=0; r.client_order_id_=request.order_id_; r.side_=Common::Side::BUY;
    *responses.tryGetNextToWriteTo()=r; responses.updateWriteIndex(); engine.processPending();
    update.depth_snapshot_.asks_[0].price_=Ctp::price(800.04); *updates.tryGetNextToWriteTo()=update; updates.updateWriteIndex(); engine.processPending();
    decide(2,JevBias::LONG,JevIntent::OPEN); CHECK(requests.size()==0); // No repricing.
    decide(3,JevBias::SHORT,JevIntent::OPEN); CHECK(requests.size()==0);
    r.type_=Exchange::ClientResponseType::FILLED; r.exec_qty_=1; r.leaves_qty_=0; r.price_=Ctp::price(800.02); r.offset_=Common::OrderOffset::OPEN;
    *responses.tryGetNextToWriteTo()=r; responses.updateWriteIndex(); engine.processPending();
    decide(4,JevBias::LONG,JevIntent::OPEN); CHECK(requests.size()==0);
    update.depth_snapshot_.bids_[0].price_=Ctp::price(800.04);
    update.depth_snapshot_.asks_[0].price_=Ctp::price(800.06);
    *updates.tryGetNextToWriteTo()=update; updates.updateWriteIndex(); engine.processPending();
    auto state=engine.buildJevEvaluationState(0,4);
    CHECK(std::abs(state.position_.unrealized_pnl_-30)<0.001);
    JevHttpClient client({}); auto json=nlohmann::json::parse(client.buildRequestBody(state));
    CHECK(json["state"]["position"]["average_entry_price"]==800.02);
    CHECK(json["state"]["depth"]["bids"][0]["price"]==800.04);
    CHECK(json["state"]["position"].contains("pnl_basis"));
    engine.updateVenueState(true,{500,500,1000},2000);
    const auto refreshed=engine.buildJevEvaluationState(0,5);
    CHECK(refreshed.account_.available_margin_==500);
    CHECK(std::abs(refreshed.position_.unrealized_pnl_-60)<0.001);
    engine.disableVenueTrading(); decide(5,JevBias::LONG,JevIntent::CLOSE); CHECK(requests.size()==0);
}

int main() {
    try { credentialChecks(); startupChecks(); tradingAndDuplicates(); shutdownAndFailures(); rejectionChecks(); engineContract();
        CHECK(Ctp::price(std::numeric_limits<double>::max())==Common::Price_INVALID);
        CHECK(Ctp::price(NAN)==Common::Price_INVALID);
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
