#pragma once

#include <sstream>
#include <string>

#include "common/lf_queue.h"
#include "common/types.h"

namespace Exchange {

enum class ClientResponseType : std::uint8_t {
    INVALID = 0,
    ACCEPTED = 1,
    CANCELED = 2,
    FILLED = 3,
    CANCEL_REJECTED = 4,
    REJECTED = 5,
};

inline auto clientResponseTypeToString(ClientResponseType type) -> std::string {
    switch (type) {
        case ClientResponseType::INVALID:
            return "INVALID";
        case ClientResponseType::ACCEPTED:
            return "ACCEPTED";
        case ClientResponseType::CANCELED:
            return "CANCELED";
        case ClientResponseType::FILLED:
            return "FILLED";
        case ClientResponseType::CANCEL_REJECTED:
            return "CANCEL_REJECTED";
        case ClientResponseType::REJECTED:
            return "REJECTED";
    }
    return "UNKNOWN";
}

struct ClientResponse {
    ClientResponseType type_ = ClientResponseType::INVALID;

    Common::ClientId client_id_ = Common::ClientId_INVALID;
    Common::TickerId ticker_id_ = Common::TickerId_INVALID;
    Common::OrderId client_order_id_ = Common::OrderId_INVALID;
    Common::OrderId venue_order_id_ = Common::OrderId_INVALID;
    Common::Side side_ = Common::Side::INVALID;
    Common::Price price_ = Common::Price_INVALID;
    Common::Qty exec_qty_ = Common::Qty_INVALID;
    Common::Qty leaves_qty_ = Common::Qty_INVALID;
    Common::OrderType order_type_ = Common::OrderType::INVALID;
    Common::OrderOffset offset_ = Common::OrderOffset::INVALID;

    auto toString() const -> std::string {
        std::stringstream stream;
        stream << "ClientResponse[type:" << clientResponseTypeToString(type_)
                      << " client:" << client_id_ << " ticker:" << ticker_id_
                      << " client-order:" << client_order_id_
                      << " venue-order:" << venue_order_id_
                      << " side:" << Common::sideToString(side_)
                      << " price:" << price_ << " exec-qty:" << exec_qty_
                      << " leaves-qty:" << leaves_qty_ << "]";
        return stream.str();
    }
};

struct SequencedClientResponse {
    std::size_t seq_num_ = 0;
    ClientResponse client_response_{};
};

using ClientResponseLFQueue = Common::LFQueue<ClientResponse>;

}  // namespace Exchange
