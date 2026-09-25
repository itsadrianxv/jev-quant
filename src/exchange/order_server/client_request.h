#pragma once

#include <sstream>
#include <string>

#include "common/lf_queue.h"
#include "common/types.h"

namespace Exchange {

enum class ClientRequestType : std::uint8_t {
    INVALID = 0,
    NEW = 1,
    CANCEL = 2,
};

inline auto clientRequestTypeToString(ClientRequestType type) -> std::string {
    switch (type) {
        case ClientRequestType::INVALID:
            return "INVALID";
        case ClientRequestType::NEW:
            return "NEW";
        case ClientRequestType::CANCEL:
            return "CANCEL";
    }
    return "UNKNOWN";
}

struct ClientRequest {
    ClientRequestType type_ = ClientRequestType::INVALID;

    Common::ClientId client_id_ = Common::ClientId_INVALID;
    Common::TickerId ticker_id_ = Common::TickerId_INVALID;
    Common::OrderId order_id_ = Common::OrderId_INVALID;
    Common::Side side_ = Common::Side::INVALID;
    Common::Price price_ = Common::Price_INVALID;
    Common::Qty qty_ = Common::Qty_INVALID;
    Common::OrderType order_type_ = Common::OrderType::INVALID;
    Common::OrderOffset offset_ = Common::OrderOffset::INVALID;

    auto toString() const -> std::string {
        std::stringstream stream;
        stream << "ClientRequest[type:" << clientRequestTypeToString(type_)
                      << " client:" << client_id_ << " ticker:" << ticker_id_
                      << " order:" << order_id_ << " side:" << Common::sideToString(side_)
                      << " price:" << price_ << " qty:" << qty_
                      << " order-type:" << Common::orderTypeToString(order_type_)
                      << " offset:" << Common::orderOffsetToString(offset_) << "]";
        return stream.str();
    }
};

struct SequencedClientRequest {
    std::size_t seq_num_ = 0;
    ClientRequest client_request_{};
};

using ClientRequestLFQueue = Common::LFQueue<ClientRequest>;

}  // namespace Exchange
