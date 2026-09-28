#include "trading/venue/simnow_venue_adapter.h"
#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>

using namespace Trading;
namespace {
std::atomic<bool> interrupted{false};
extern "C" void signalHandler(int) { interrupted=true; }
}

// Deliberately not registered with ctest: this executable places SimNow orders.
int main(int argc,char** argv) {
    if(argc!=3 || std::string(argv[1])!="--place-orders") {
        std::cerr<<"Usage: simnow_live_smoke --place-orders <config.json>\n";
        return 2;
    }
    try {
        nlohmann::json document; std::ifstream file(argv[2]); file>>document;
        const auto ticker=document.at("runtime").at("instrument").at("ticker_id").get<Common::TickerId>();
        auto config=SimNowConfig::fromJson(document.at("venue"),ticker);
        Exchange::ClientRequestLFQueue requests(4096);
        Exchange::ClientResponseLFQueue responses(4096);
        Exchange::MarketUpdateLFQueue updates(4096);
        MarketDataConsumer market(1,&updates);
        OrderGateway gateway(1,&requests,&responses);
        std::atomic<bool> ready{false};
        SimNowVenueAdapter adapter(&market,&gateway,config,[&](bool r,AccountState,double){ready=r;});
        std::signal(SIGINT,signalHandler); std::signal(SIGTERM,signalHandler);
        adapter.start();
        Exchange::DepthSnapshot depth;
        const auto started=std::chrono::steady_clock::now();
        int phase=0;
        bool round_trip=false, cancel_confirmed=false;
        const auto send=[&](Common::OrderId id,Common::Side side,Common::OrderOffset offset,Common::Price price){
            Exchange::ClientRequest r; r.type_=Exchange::ClientRequestType::NEW; r.client_id_=1;
            r.ticker_id_=ticker; r.order_id_=id; r.side_=side; r.offset_=offset;
            r.price_=price; r.qty_=1; r.order_type_=Common::OrderType::LIMIT;
            *requests.tryGetNextToWriteTo()=r; requests.updateWriteIndex();
        };
        while(!interrupted && !adapter.failed() && std::chrono::steady_clock::now()-started<std::chrono::seconds(120)) {
            while(auto* u=updates.getNextToRead()) {depth=u->depth_snapshot_; updates.updateReadIndex();}
            while(auto* r=responses.getNextToRead()) {
                const auto response=*r; responses.updateReadIndex();
                if(response.type_==Exchange::ClientResponseType::REJECTED || response.type_==Exchange::ClientResponseType::CANCEL_REJECTED)
                    throw std::runtime_error("Smoke order rejected");
                if(response.type_==Exchange::ClientResponseType::FILLED && response.client_order_id_==1) phase=2;
                if(response.type_==Exchange::ClientResponseType::FILLED && response.client_order_id_==2) {round_trip=true; phase=4;}
                if(response.type_==Exchange::ClientResponseType::ACCEPTED && response.client_order_id_==3) {
                    Exchange::ClientRequest cancel; cancel.type_=Exchange::ClientRequestType::CANCEL;
                    cancel.client_id_=1; cancel.ticker_id_=ticker; cancel.order_id_=3;
                    *requests.tryGetNextToWriteTo()=cancel; requests.updateWriteIndex();
                }
                if(response.client_order_id_==3 && response.type_==Exchange::ClientResponseType::CANCELED) cancel_confirmed=true;
                if(response.client_order_id_==3 && response.type_==Exchange::ClientResponseType::FILLED)
                    throw std::runtime_error("Cancellation probe filled; inspect remaining position");
            }
            if(cancel_confirmed) break;
            if(ready && phase==0 && depth.asks_[0].price_!=Common::Price_INVALID) {
                send(1,Common::Side::BUY,Common::OrderOffset::OPEN,depth.asks_[0].price_); phase=1;
            }
            if(ready && phase==2 && depth.bids_[0].price_!=Common::Price_INVALID) {
                send(2,Common::Side::SELL,Common::OrderOffset::CLOSE_TODAY,depth.bids_[0].price_); phase=3;
            }
            if(ready && phase==4 && depth.lower_limit_price_!=Common::Price_INVALID) {
                send(3,Common::Side::BUY,Common::OrderOffset::OPEN,depth.lower_limit_price_); phase=5;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        adapter.stop();
        std::cout<<"event=simnow_smoke_result round_trip="<<round_trip<<" canceled="<<cancel_confirmed
                 <<" position="<<adapter.position()<<" failed="<<adapter.failed()<<'\n';
        return round_trip && cancel_confirmed && adapter.position()==0 && !adapter.failed() ? 0 : 1;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
