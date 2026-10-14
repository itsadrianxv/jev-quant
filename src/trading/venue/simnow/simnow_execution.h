#pragma once
#include "ctp_transport.h"
#include "simnow_control.h"
#include "trading/order_gw/order_gateway.h"
#include "trading/strategy/jev_decision.h"
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <set>

namespace Trading {
// Mutable execution state belongs to the OrderGateway worker, or its owner after join.
class SimNowExecution {
  public:
    using StateSink = std::function<void(bool, AccountState, double)>;
    SimNowExecution(OrderGateway* gateway, const SimNowConfig& config,
                    Ctp::Transport* transport, SimNowControl& control, StateSink sink);
    void enqueue(Ctp::Event event) noexcept;
    void handleRequest(const Exchange::ClientRequest& request) noexcept;
    void pollOrders() noexcept;
    void state(bool enabled);
    bool outstanding() const;
    int position() const noexcept { return net_position.load(); }

  private:
    using Clock = std::chrono::steady_clock;
    using Kind = Ctp::Kind;
    OrderGateway* gateway;
    const SimNowConfig& config;
    Ctp::Transport* transport;
    SimNowControl& control;
    StateSink state_sink;
    Common::LFQueue<Ctp::Event> trader_events;
    std::atomic<int> net_position{0};
    bool ready = false, trader_connected = false;
    bool instrument_seen=false, account_seen=false, startup_done=false;
    int next_id=0, query_id=0, front=0, session=0;
    std::uint64_t next_ref=0;
    std::string trading_day;
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

    void fail(const std::string& reason);
    void request(Kind kind);
    void publish(Order& o,Exchange::ClientResponseType type,int qty=0,Common::Price price=Common::Price_INVALID);
    void finish(Order& o);
    void cancel(Order& o, const Exchange::ClientRequest* observed = nullptr);
    void submit(const Exchange::ClientRequest& r);
    void orderEvent(const CThostFtdcOrderField& p);
    void tradeEvent(const CThostFtdcTradeField& p);
    void event(const Ctp::Event& e);
};
}  // namespace Trading
