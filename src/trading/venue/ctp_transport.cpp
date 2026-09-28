#include "simnow_venue_adapter.h"
#include "ThostFtdcTraderApi.h"
#include "ThostFtdcMdApi.h"
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace Trading::Ctp {
namespace {
template<std::size_t N> void put(char (&out)[N], const std::string& value) {
    if (value.size() >= N) throw std::invalid_argument("CTP field exceeds SDK width");
    std::memcpy(out, value.c_str(), value.size() + 1);
}
class Native final : public Transport {
    struct Trader final : CThostFtdcTraderSpi {
        Native& owner;
        explicit Trader(Native& n) : owner(n) {}
        void OnFrontConnected() override { owner.emit(Kind::TraderConnected); }
        void OnFrontDisconnected(int e) override { owner.emit(Kind::Disconnected, nullptr, 0, true, e); }
        void OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* e, int id, bool last) override { owner.emit(Kind::Auth,e,id,last); }
        void OnRspUserLogin(CThostFtdcRspUserLoginField* p, CThostFtdcRspInfoField* e, int id, bool last) override { owner.data(Kind::Login,p,e,id,last); }
        void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*, CThostFtdcRspInfoField* e, int id, bool last) override { owner.emit(Kind::Settlement,e,id,last); }
        void OnRspQryInstrument(CThostFtdcInstrumentField* p, CThostFtdcRspInfoField* e, int id, bool last) override { owner.data(Kind::Instrument,p,e,id,last); }
        void OnRspQryTradingAccount(CThostFtdcTradingAccountField* p, CThostFtdcRspInfoField* e, int id, bool last) override { owner.data(Kind::Account,p,e,id,last); }
        void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* p, CThostFtdcRspInfoField* e, int id, bool last) override { owner.data(Kind::Position,p,e,id,last); }
        void OnRspQryOrder(CThostFtdcOrderField* p, CThostFtdcRspInfoField* e, int id, bool last) override { owner.data(Kind::OrderQuery,p,e,id,last); }
        void OnRtnOrder(CThostFtdcOrderField* p) override { owner.data(Kind::Order,p); }
        void OnRtnTrade(CThostFtdcTradeField* p) override { owner.data(Kind::Trade,p); }
        void OnRspOrderInsert(CThostFtdcInputOrderField* p, CThostFtdcRspInfoField* e, int id, bool last) override { owner.data(Kind::InsertError,p,e,id,last); }
        void OnErrRtnOrderInsert(CThostFtdcInputOrderField* p, CThostFtdcRspInfoField* e) override { owner.data(Kind::InsertError,p,e); }
        void OnRspOrderAction(CThostFtdcInputOrderActionField* p, CThostFtdcRspInfoField* e, int id, bool last) override { owner.data(Kind::CancelError,p,e,id,last); }
        void OnErrRtnOrderAction(CThostFtdcOrderActionField* p, CThostFtdcRspInfoField* e) override {
            CThostFtdcInputOrderActionField out{};
            if (p) { put(out.OrderRef,p->OrderRef); out.FrontID=p->FrontID; out.SessionID=p->SessionID; }
            owner.data(Kind::CancelError,&out,e);
        }
        void OnRspError(CThostFtdcRspInfoField* e,int id,bool last) override { owner.emit(Kind::Error,e,id,last); }
    } trader_spi_{*this};
    struct Market final : CThostFtdcMdSpi {
        Native& owner;
        explicit Market(Native& n) : owner(n) {}
        void OnFrontConnected() override { owner.emit(Kind::MarketConnected); }
        void OnFrontDisconnected(int e) override { owner.emit(Kind::Disconnected,nullptr,0,true,e); }
        void OnRspUserLogin(CThostFtdcRspUserLoginField* p,CThostFtdcRspInfoField* e,int id,bool last) override { owner.data(Kind::MarketLogin,p,e,id,last); }
        void OnRspSubMarketData(CThostFtdcSpecificInstrumentField*,CThostFtdcRspInfoField* e,int id,bool last) override { owner.emit(Kind::Subscription,e,id,last); }
        void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* p) override { owner.data(Kind::Depth,p); }
        void OnRspError(CThostFtdcRspInfoField* e,int id,bool last) override { owner.emit(Kind::Error,e,id,last); }
    } market_spi_{*this};
    CThostFtdcTraderApi* trader_ = nullptr;
    CThostFtdcMdApi* market_ = nullptr;
    SimNowConfig config_;
    Sink sink_;
    void emit(Kind kind,CThostFtdcRspInfoField* e=nullptr,int id=0,bool last=true,int error=0) {
        sink_({kind,id,e ? e->ErrorID : error,last,{}});
    }
    template<class T> void data(Kind kind,T* p,CThostFtdcRspInfoField* e=nullptr,int id=0,bool last=true) {
        Event event{kind,id,e ? e->ErrorID : 0,last,{}};
        if(p) event.payload=*p;
        sink_(std::move(event));
    }
  public:
    ~Native() override { stop(); }
    void start(const SimNowConfig& c,Sink sink) override {
        config_=c; sink_=std::move(sink);
        const auto td=c.flow_directory+"/trader/", md=c.flow_directory+"/market/";
        std::filesystem::create_directories(td); std::filesystem::create_directories(md);
        trader_=CThostFtdcTraderApi::CreateFtdcTraderApi(td.c_str());
        market_=CThostFtdcMdApi::CreateFtdcMdApi(md.c_str());
        if(!trader_ || !market_) throw std::runtime_error("CTP API creation failed");
        trader_->RegisterSpi(&trader_spi_);
        trader_->SubscribePrivateTopic(THOST_TERT_QUICK);
        trader_->SubscribePublicTopic(THOST_TERT_QUICK);
        trader_->RegisterFront(config_.trading_front.data());
        market_->RegisterSpi(&market_spi_); market_->RegisterFront(config_.market_front.data());
        trader_->Init(); market_->Init();
    }
    void stop() noexcept override {
        if(trader_) { trader_->RegisterSpi(nullptr); trader_->Release(); trader_=nullptr; }
        if(market_) { market_->RegisterSpi(nullptr); market_->Release(); market_=nullptr; }
    }
    int request(Kind kind,int id) override {
        switch(kind) {
        case Kind::Auth: {
            CThostFtdcReqAuthenticateField p{};
            put(p.BrokerID,config_.broker); put(p.UserID,config_.user);
            put(p.AppID,config_.app_id); put(p.AuthCode,config_.auth_code);
            return trader_->ReqAuthenticate(&p,id);
        }
        case Kind::Login: case Kind::MarketLogin: {
            CThostFtdcReqUserLoginField p{};
            put(p.BrokerID,config_.broker); put(p.UserID,config_.user); put(p.Password,config_.password);
            return kind==Kind::Login ? trader_->ReqUserLogin(&p,id) : market_->ReqUserLogin(&p,id);
        }
        case Kind::Settlement: {
            CThostFtdcSettlementInfoConfirmField p{};
            put(p.BrokerID,config_.broker); put(p.InvestorID,config_.user);
            return trader_->ReqSettlementInfoConfirm(&p,id);
        }
        case Kind::Instrument: {
            CThostFtdcQryInstrumentField p{}; put(p.InstrumentID,config_.instrument);
            return trader_->ReqQryInstrument(&p,id);
        }
        case Kind::Account: {
            CThostFtdcQryTradingAccountField p{}; put(p.BrokerID,config_.broker); put(p.InvestorID,config_.user);
            put(p.CurrencyID,"CNY"); return trader_->ReqQryTradingAccount(&p,id);
        }
        case Kind::Position: {
            CThostFtdcQryInvestorPositionField p{}; put(p.BrokerID,config_.broker); put(p.InvestorID,config_.user);
            return trader_->ReqQryInvestorPosition(&p,id);
        }
        case Kind::OrderQuery: {
            CThostFtdcQryOrderField p{}; put(p.BrokerID,config_.broker); put(p.InvestorID,config_.user);
            return trader_->ReqQryOrder(&p,id);
        }
        case Kind::Subscription: {
            char* instrument=config_.instrument.data(); return market_->SubscribeMarketData(&instrument,1);
        }
        default: throw std::logic_error("Unsupported CTP request");
        }
    }
    int insert(const CThostFtdcInputOrderField& value,int id) override { auto p=value; return trader_->ReqOrderInsert(&p,id); }
    int cancel(const CThostFtdcInputOrderActionField& value,int id) override { auto p=value; return trader_->ReqOrderAction(&p,id); }
};
}
std::unique_ptr<Transport> makeTransport() { return std::make_unique<Native>(); }
}
