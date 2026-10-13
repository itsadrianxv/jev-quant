#pragma once

#include <cstddef>
#include <stdexcept>
#include <unordered_map>

#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "trading/order_gw/order_gateway.h"

namespace Tests {

class SimulatedVenue final {
  public:
    explicit SimulatedVenue(Trading::OrderGateway* gateway)
            : gateway_(gateway) {
        if (gateway_ == nullptr) {
            throw std::invalid_argument("SimulatedVenue gateway must not be null");
        }
        gateway_->setRequestHandler(
                [this](std::size_t sequence, const Exchange::ClientRequest& request) {
                    onRequest(sequence, request);
                });
    }

    auto fill(Common::OrderId order_id, Common::Price price,
                        Common::Qty exec_qty) -> void {
        const auto iterator = live_orders_.find(order_id);
        if (iterator == live_orders_.end()) {
            throw std::invalid_argument("Unknown simulated order");
        }

        auto response = baseResponse(iterator->second);
        response.type_ = Exchange::ClientResponseType::FILLED;
        response.price_ = price;
        response.exec_qty_ = exec_qty;
        response.leaves_qty_ = iterator->second.qty_ - exec_qty;
        if (response.leaves_qty_ == 0) {
            live_orders_.erase(iterator);
        } else {
            iterator->second.qty_ = response.leaves_qty_;
        }
        publish(response);
    }

  private:
    struct LiveOrder {
        Exchange::ClientRequest request_{};
        Common::Qty qty_ = 0;
    };

    Trading::OrderGateway* gateway_ = nullptr;
    std::size_t next_response_seq_num_ = 1;
    std::unordered_map<Common::OrderId, LiveOrder> live_orders_;

    auto onRequest(std::size_t, const Exchange::ClientRequest& request) -> void {
        if (request.type_ == Exchange::ClientRequestType::NEW) {
            live_orders_[request.order_id_] = LiveOrder{request, request.qty_};
            auto response = baseResponse(live_orders_.at(request.order_id_));
            response.type_ = Exchange::ClientResponseType::ACCEPTED;
            response.exec_qty_ = 0;
            response.leaves_qty_ = request.qty_;
            publish(response);
            return;
        }

        if (request.type_ == Exchange::ClientRequestType::CANCEL) {
            const auto iterator = live_orders_.find(request.order_id_);
            if (iterator == live_orders_.end()) {
                auto response = baseResponse(LiveOrder{request, 0});
                response.type_ = Exchange::ClientResponseType::CANCEL_REJECTED;
                publish(response);
                return;
            }
            auto response = baseResponse(iterator->second);
            response.type_ = Exchange::ClientResponseType::CANCELED;
            response.exec_qty_ = 0;
            response.leaves_qty_ = 0;
            live_orders_.erase(iterator);
            publish(response);
        }
    }

    auto baseResponse(const LiveOrder& order) const -> Exchange::ClientResponse {
        Exchange::ClientResponse response;
        response.client_id_ = order.request_.client_id_;
        response.ticker_id_ = order.request_.ticker_id_;
        response.client_order_id_ = order.request_.order_id_;
        response.venue_order_id_ = order.request_.order_id_ + 1000000;
        response.side_ = order.request_.side_;
        response.price_ = order.request_.price_;
        response.leaves_qty_ = order.qty_;
        response.order_type_ = order.request_.order_type_;
        response.offset_ = order.request_.offset_;
        return response;
    }

    auto publish(const Exchange::ClientResponse& response) -> void {
        gateway_->publishClientResponse(next_response_seq_num_++, response);
    }
};

}  // namespace Tests
