#include "trading/venue/simnow/ctp_mapping.h"
#include "trading/venue/simnow/ctp_transport.h"
#include <gtest/gtest.h>

#include "trading/venue/simnow/simnow_venue_adapter.h"
#include "trading/market_data/market_data_consumer.h"
#include "trading/order_gw/order_gateway.h"
#include "trading/performance/measurement.h"
#include "trading/strategy/trade_engine.h"
#include "trading/strategy/jev_http_client.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace Trading;
using namespace std::chrono_literals;
using Kind = Trading::Ctp::Kind;

// Integration harness helper: bounds every wait with an explicit deadline and
// reports the observed state in the failure message when the deadline passes.
void waitUntil(std::function<bool()> ready, std::function<std::string()> describe,
               std::chrono::milliseconds budget = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!ready()) {
        if (std::chrono::steady_clock::now() > deadline) {
            throw std::runtime_error("wait exceeded " + std::to_string(budget.count()) +
                                     "ms; observed: " + describe());
        }
        std::this_thread::sleep_for(1ms);
    }
}

template <std::size_t N>
void text(char (&dest)[N], const char* src) {
    const auto length = std::strlen(src);
    if (length >= N) throw std::length_error("fixture CTP field exceeds SDK width");
    std::memcpy(dest, src, length + 1);
}

struct Fake final : Ctp::Transport {
    Measurement* measurement = nullptr;
    Sink trader, market;
    std::mutex mutex;
    std::vector<CThostFtdcInputOrderField> sent;
    std::vector<CThostFtdcInputOrderActionField> canceled;
    std::atomic<int> query_count{0};
    bool dirty = false, working = false, reject_insert = false, cancel_fill = false,
         no_cancel = false;
    bool no_instrument = false, query_throttle = false, flood = false;
    int market_login_id = -1;
    const char* market_day = "20260928";
    std::function<void()> before_insert;
    std::atomic<int> subscriptions{0};
    static bool marketSide(Kind k) {
        return k == Kind::MarketConnected || k == Kind::MarketLogin ||
               k == Kind::Subscription || k == Kind::Depth;
    }
    void start(const SimNowConfig& config, Sink t, Sink m) override {
        measurement = config.measurement;
        trader = std::move(t);
        market = std::move(m);
        if (flood) {
            // Exhaust the bounded SPSC queue while no consumer is running yet:
            // every event beyond capacity must raise the overflow flag.
            Ctp::Event event;
            event.kind = Kind::Order;
            for (int i = 0; i < 4098; ++i) trader(event);
            return;
        }
        emit(Kind::TraderConnected);
        emit(Kind::MarketConnected);
    }
    void stop() noexcept override {}
    void emit(Kind k, Ctp::Payload p = {}, int id = 0, int error = 0, bool last = true) {
        Ctp::Event event{k, id, error, last, std::move(p)};
        if (k == Kind::Depth && measurement) event.trace = measurement->source(Common::nowSteadyNs());
        (marketSide(k) ? market : trader)(std::move(event));
    }
    CThostFtdcDepthMarketDataField depth() {
        CThostFtdcDepthMarketDataField p{};
        text(p.InstrumentID, "au2612");
        text(p.TradingDay, market_day);
        p.BidPrice1 = 800;
        p.AskPrice1 = 800.02;
        p.BidVolume1 = 10;
        p.AskVolume1 = 10;
        p.LastPrice = 800;
        p.Volume = 10;
        return p;
    }
    int request(Kind k, int id) override {
        switch (k) {
        case Kind::Login:
        case Kind::MarketLogin: {
            CThostFtdcRspUserLoginField p{};
            p.FrontID = 7;
            p.SessionID = 8;
            text(p.TradingDay, "20260928");
            text(p.MaxOrderRef, "20");
            emit(k, p, k == Kind::MarketLogin && market_login_id >= 0 ? market_login_id : id);
            break;
        }
        case Kind::Instrument: {
            ++query_count;
            if (query_throttle) {
                query_throttle = false;
                return -3;
            }
            if (no_instrument) {
                emit(k, {}, id);
                break;
            }
            CThostFtdcInstrumentField p{};
            text(p.InstrumentID, "au2612");
            text(p.ExchangeID, "SHFE");
            p.PriceTick = 0.02;
            p.VolumeMultiple = 1000;
            p.ProductClass = THOST_FTDC_PC_Futures;
            p.IsTrading = 1;
            p.MinLimitOrderVolume = 1;
            p.MaxLimitOrderVolume = 100;
            emit(k, p, id);
            break;
        }
        case Kind::Account: {
            CThostFtdcTradingAccountField p{};
            text(p.CurrencyID, "CNY");
            p.Balance = 1000000;
            p.Available = 900000;
            p.CurrMargin = 100000;
            emit(k, p, id);
            break;
        }
        case Kind::Position: {
            if (dirty) {
                CThostFtdcInvestorPositionField p{};
                p.Position = 1;
                emit(k, p, id);
            } else emit(k, {}, id);
            break;
        }
        case Kind::OrderQuery: {
            if (working) {
                CThostFtdcOrderField p{};
                p.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
                emit(k, p, id);
            } else emit(k, {}, id);
            break;
        }
        case Kind::Subscription:
            ++subscriptions;
            emit(k, {}, id);
            emit(Kind::Depth, depth());
            break;
        default: emit(k, {}, id); break;
        }
        return 0;
    }
    int insert(const CThostFtdcInputOrderField& p, int) override {
        if (before_insert) before_insert();
        std::scoped_lock lock(mutex);
        sent.push_back(p);
        return reject_insert ? -1 : 0;
    }
    CThostFtdcInputOrderField latest() {
        std::scoped_lock lock(mutex);
        return sent.back();
    }
    std::size_t count() {
        std::scoped_lock lock(mutex);
        return sent.size();
    }
    CThostFtdcOrderField order(const CThostFtdcInputOrderField& in, char status, int volume = 0) {
        CThostFtdcOrderField p{};
        text(p.OrderRef, in.OrderRef);
        text(p.InstrumentID, in.InstrumentID);
        text(p.OrderSysID, "000000001");
        text(p.ExchangeID, "SHFE");
        p.FrontID = 7;
        p.SessionID = 8;
        p.OrderStatus = status;
        p.VolumeTraded = volume;
        p.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
        return p;
    }
    CThostFtdcTradeField trade(const CThostFtdcInputOrderField& in, const char* id = "trade1") {
        CThostFtdcTradeField p{};
        text(p.OrderRef, in.OrderRef);
        text(p.InstrumentID, in.InstrumentID);
        text(p.TradingDay, "20260928");
        text(p.TradeID, id);
        text(p.OrderSysID, "000000001");
        text(p.ExchangeID, "SHFE");
        p.Volume = 1;
        p.Price = in.LimitPrice;
        p.Direction = in.Direction;
        p.OffsetFlag = in.CombOffsetFlag[0];
        return p;
    }
    int cancel(const CThostFtdcInputOrderActionField& p, int) override {
        CThostFtdcInputOrderField in;
        {
            std::scoped_lock lock(mutex);
            canceled.push_back(p);
            in = sent.back();
        }
        if (no_cancel) return 0;
        if (cancel_fill) {
            emit(Kind::Order, order(in, THOST_FTDC_OST_AllTraded, 1));
            emit(Kind::Trade, trade(in));
        } else emit(Kind::Order, order(in, THOST_FTDC_OST_Canceled));
        return 0;
    }
};

class SimNowCredentials : public ::testing::Test {
  protected:
    class EnvironmentGuard {
      public:
        EnvironmentGuard(const char* name) : key_(name), present_(std::getenv(name) != nullptr) {
            if (present_) saved_ = std::getenv(name);
            unsetenv(name);
        }
        ~EnvironmentGuard() {
            if (present_) setenv(key_.c_str(), saved_.c_str(), 1);
            else unsetenv(key_.c_str());
        }
        EnvironmentGuard(const EnvironmentGuard&) = delete;
        EnvironmentGuard& operator=(const EnvironmentGuard&) = delete;

      private:
        std::string key_;
        std::string saved_;
        bool present_;
    };

    void SetUp() override {
        static std::atomic<int> sequence{0};
        dotenv_ = std::filesystem::temp_directory_path() /
                  ("simnow-dotenv-" + std::to_string(++sequence));
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove(dotenv_, error);
    }

    void writeDotenv(const std::string& content) {
        std::ofstream out(dotenv_, std::ios::binary);
        out << content;
    }

    SimNowConfig read() {
        const nlohmann::json json = {{"trading_front", "tcp://test:1"},
                                     {"market_front", "tcp://test:2"},
                                     {"instrument", "rb2610"}};
        return SimNowConfig::fromJson(json, 0, dotenv_.string());
    }

    void expectCredentialFailure(const std::string& expected) {
        try {
            read();
        } catch (const std::runtime_error& error) {
            const std::string message = error.what();
            EXPECT_NE(std::string::npos, message.find(expected))
                    << "observed message: " << message;
            EXPECT_EQ(std::string::npos, message.find("private-value"))
                    << "observed message: " << message;
            return;
        }
        ADD_FAILURE() << "expected a credential failure mentioning " << expected;
    }

    EnvironmentGuard user_{"SIMNOW_USER_ID"};
    EnvironmentGuard password_{"SIMNOW_PASSWORD"};
    std::filesystem::path dotenv_;
};

TEST_F(SimNowCredentials, ReportsMissingCredentialsWithoutPrivateValues) {
    expectCredentialFailure("SIMNOW_USER_ID");
}

TEST_F(SimNowCredentials, ParsesQuotedAndCommentedDotenvValues) {
    writeDotenv("\xEF\xBB\xBF# Test fixture\r\nexport SIMNOW_USER_ID = 'file-user' # comment\r\n"
                "UNRELATED_KEY=ignored\r\nSIMNOW_PASSWORD=\"private-value#=$\"\r\n");

    const auto config = read();

    EXPECT_EQ("file-user", config.user);
    EXPECT_EQ("private-value#=$", config.password);
    EXPECT_EQ(nullptr, std::getenv("SIMNOW_USER_ID"));
}

TEST_F(SimNowCredentials, PrefersTheProcessEnvironmentOverTheDotenv) {
    writeDotenv("SIMNOW_PASSWORD=\"private-value#=$\"\r\n");
    setenv("SIMNOW_USER_ID", "env-user", 1);

    auto config = read();

    EXPECT_EQ("env-user", config.user);
    EXPECT_EQ("private-value#=$", config.password);

    setenv("SIMNOW_PASSWORD", "env-password", 1);
    config = read();

    EXPECT_EQ("env-user", config.user);
    EXPECT_EQ("env-password", config.password);
}

TEST_F(SimNowCredentials, TreatsAnEmptyEnvironmentValueAsUnset) {
    setenv("SIMNOW_USER_ID", "env-user", 1);
    setenv("SIMNOW_PASSWORD", "", 1);
    writeDotenv("SIMNOW_PASSWORD=plain=value # comment\n");

    EXPECT_EQ("plain=value", read().password);
}

TEST_F(SimNowCredentials, RejectsUnterminatedDotenvQuotesWithoutValueLeaks) {
    setenv("SIMNOW_USER_ID", "env-user", 1);
    setenv("SIMNOW_PASSWORD", "", 1);
    writeDotenv("SIMNOW_PASSWORD='private-value\n");

    expectCredentialFailure("Invalid dotenv value for SIMNOW_PASSWORD");
}

TEST_F(SimNowCredentials, RejectsEmptyDotenvValues) {
    setenv("SIMNOW_USER_ID", "env-user", 1);
    setenv("SIMNOW_PASSWORD", "", 1);
    writeDotenv("SIMNOW_PASSWORD=\n");

    expectCredentialFailure("SIMNOW_PASSWORD");
}

class SimNowAdapter : public ::testing::Test {
  protected:
    void arrange(std::function<void(Fake&)> setup) { setup_ = std::move(setup); }

    void prepareAdapter() {
        auto transport = std::make_unique<Fake>();
        fake_ = transport.get();
        if (setup_) setup_(*fake_);
        SimNowConfig config;
        config.measurement = measurement_;
        config.instrument = "au2612";
        config.user = "test";
        config.password = "test";
        config.auth_code = "test";
        config.trading_front = "tcp://fake:1";
        config.market_front = "tcp://fake:2";
        config.query_interval = 1ms;
        config.startup_timeout = 500ms;
        config.shutdown_timeout = 30ms;
        adapter_ = std::make_unique<SimNowVenueAdapter>(
                &market_, &gateway_, config,
                [this](bool ready, AccountState, double) { ready_.store(ready); },
                std::move(transport));
        adapter_->prepare();
    }

    void startAdapter() {
        try {
            prepareAdapter();
            market_.start();
            gateway_.start();
            adapter_->waitUntilReady();
        } catch (...) {
            stopAdapter();
            throw;
        }
    }

    void stopAdapter() {
        market_.stop();
        if (adapter_) adapter_->quiesce();
        gateway_.stop();
        if (adapter_) adapter_->stop();
    }

    void TearDown() override { stopAdapter(); }

    void send(Common::OrderId id = 1,
              Common::OrderOffset offset = Common::OrderOffset::OPEN,
              Common::Side side = Common::Side::BUY, double price = 800.02) {
        Exchange::ClientRequest request;
        request.type_ = Exchange::ClientRequestType::NEW;
        request.client_id_ = 1;
        request.ticker_id_ = 0;
        request.order_id_ = id;
        request.offset_ = offset;
        request.side_ = side;
        request.order_type_ = Common::OrderType::LIMIT;
        request.qty_ = 1;
        request.price_ = Ctp::price(price);
        request.trace_ = request_trace_;
        if (request.trace_.source_id) request.trace_.t[4] = Common::nowSteadyNs();
        *requests_.tryGetNextToWriteTo() = request;
        requests_.updateWriteIndex();
    }

    Exchange::ClientResponse response() {
        waitUntil([&] { return responses_.size() > 0; },
                  [&] {
                      return "responses=" + std::to_string(responses_.size()) +
                             " adapterFailed=" + std::to_string(adapter_->failed());
                  });
        auto response = *responses_.getNextToRead();
        responses_.updateReadIndex();
        return response;
    }

    Exchange::DepthSnapshot firstDepthSnapshot() {
        waitUntil([&] { return updates_.size() > 0; },
                  [&] { return "marketUpdates=" + std::to_string(updates_.size()); });
        auto update = *updates_.getNextToRead();
        updates_.updateReadIndex();
        return update.depth_snapshot_;
    }

    void awaitFirstInsert() {
        waitUntil([&] { return fake_->count() == 1; },
                  [&] { return "inserts=" + std::to_string(fake_->count()); });
    }

    void awaitFailure() {
        waitUntil([&] { return adapter_->failed(); },
                  [&] { return "adapterFailed=" + std::to_string(adapter_->failed()); });
    }

    Exchange::ClientRequestLFQueue requests_{64};
    Measurement* measurement_ = nullptr;
    Common::PerformanceTrace request_trace_{};
    Exchange::ClientResponseLFQueue responses_{64};
    Exchange::MarketUpdateLFQueue updates_{128};
    MarketDataConsumer market_{1, &updates_};
    OrderGateway gateway_{1, &requests_, &responses_};
    std::function<void(Fake&)> setup_;
    Fake* fake_ = nullptr;
    std::unique_ptr<SimNowVenueAdapter> adapter_;
    std::atomic<bool> ready_{false};
};

TEST_F(SimNowAdapter, PreservesCallbackSourceAcrossMarketWorkerPublication) {
    Measurement measurement(8, 60000000000ULL, 60000000000ULL, false);
    measurement_ = &measurement;
    measurement.arm();
    startAdapter();
    ASSERT_GT(updates_.size(), 0U);
    const auto trace = updates_.getNextToRead()->trace_;
    updates_.updateReadIndex();
    stopAdapter();
    EXPECT_GT(trace.source_id, 0U);
    EXPECT_GT(trace.t[0], 0U);
    EXPECT_GE(trace.t[1], trace.t[0]);
    EXPECT_GE(trace.t[2], trace.t[1]);
    EXPECT_EQ(trace.t[3], 0U);
    adapter_.reset();
}

TEST_F(SimNowAdapter, CapturesSuccessfulSubmissionAfterGatewayAndAdapterBoundaries) {
    Measurement measurement(8, 60000000000ULL, 60000000000ULL, false);
    measurement_ = &measurement;
    measurement.arm();
    startAdapter();
    request_trace_ = measurement.source(Common::nowSteadyNs());
    request_trace_.evaluation_id = 7;
    for (int i = 1; i < 4; ++i) request_trace_.t[i] = Common::nowSteadyNs();
    send();
    awaitFirstInsert();
    stopAdapter(); // The collector has a single writer; inspect only after join.
    ASSERT_EQ(measurement.size(), 1U);
    const auto& sample = measurement.sample(0);
    EXPECT_EQ(sample.trace.evaluation_id, 7U);
    for (int i = 1; i < 8; ++i) EXPECT_GE(sample.trace.t[i], sample.trace.t[i-1]);
    EXPECT_GT(sample.trace.t[7], 0U);
    adapter_.reset();
}

TEST_F(SimNowAdapter, ImmediateCtpFailureDoesNotBecomeAPerformanceSample) {
    Measurement measurement(8, 60000000000ULL, 60000000000ULL, false);
    measurement_ = &measurement;
    measurement.arm();
    arrange([](Fake& fake) { fake.reject_insert = true; });
    startAdapter();
    request_trace_ = measurement.source(Common::nowSteadyNs());
    request_trace_.evaluation_id = 7;
    for (int i = 1; i < 4; ++i) request_trace_.t[i] = Common::nowSteadyNs();
    send();
    EXPECT_EQ(response().type_, Exchange::ClientResponseType::REJECTED);
    stopAdapter();
    EXPECT_EQ(measurement.size(), 0U);
    adapter_.reset();
}

TEST_F(SimNowAdapter, PrepareLeavesBothBusinessWorkersStopped) {
    prepareAdapter();
    EXPECT_FALSE(market_.running());
    EXPECT_FALSE(gateway_.running());
    EXPECT_FALSE(ready_.load());
    EXPECT_EQ(0, fake_->query_count.load());
    EXPECT_EQ(0, fake_->subscriptions.load());
    EXPECT_THROW(adapter_->waitUntilReady(), std::logic_error);
    EXPECT_THROW(adapter_->prepare(), std::logic_error);
    stopAdapter();
    EXPECT_FALSE(adapter_->failed());
}

TEST_F(SimNowAdapter, CleansUpWhenOnlyTheMarketWorkerStarted) {
    prepareAdapter();
    try {
        market_.start();
        throw std::runtime_error("Injected startup failure before gateway start");
    } catch (const std::runtime_error&) {
        stopAdapter();
    }
    EXPECT_FALSE(market_.running());
    EXPECT_FALSE(gateway_.running());
    EXPECT_FALSE(adapter_->running());
    EXPECT_FALSE(adapter_->failed());
    adapter_.reset(); // No callback may outlive its target.
}

TEST_F(SimNowAdapter, QuiesceLeavesWorkerJoinsToTheCaller) {
    startAdapter();
    market_.stop();
    adapter_->quiesce();
    EXPECT_TRUE(gateway_.running());
    EXPECT_FALSE(ready_.load());
    EXPECT_THROW(adapter_->stop(), std::logic_error);
    stopAdapter();
    EXPECT_FALSE(gateway_.running());
    EXPECT_FALSE(adapter_->running());
}

TEST_F(SimNowAdapter, SubscribesToTheConfiguredInstrumentOnStartup) {
    arrange([](Fake& fake) { fake.market_login_id = 0; });
    startAdapter();

    EXPECT_TRUE(ready_.load());
    EXPECT_EQ(1, fake_->subscriptions.load());

    fake_->emit(Kind::MarketLogin, CThostFtdcRspUserLoginField{}, 0);
    stopAdapter();

    EXPECT_FALSE(adapter_->failed());
    EXPECT_EQ(1, fake_->subscriptions.load());
}

TEST_F(SimNowAdapter, ProcessesQuotesWhileOrderSubmissionIsBlocked) {
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto release_future = release.get_future();
    arrange([&](Fake& fake) {
        fake.before_insert = [&] {
            entered.set_value();
            EXPECT_EQ(std::future_status::ready, release_future.wait_for(5s));
        };
    });
    startAdapter();
    firstDepthSnapshot();
    send();
    const auto entered_status = entered_future.wait_for(2s);
    EXPECT_EQ(std::future_status::ready, entered_status);
    auto depth = fake_->depth();
    depth.BidPrice1 = 799;
    fake_->emit(Kind::Depth, depth);
    // Release the blocked order worker even if observing the market update fails.
    try {
        EXPECT_EQ(Ctp::price(799), firstDepthSnapshot().bids_[0].price_);
    } catch (...) {
        release.set_value();
        stopAdapter();
        throw;
    }
    release.set_value();
    awaitFirstInsert();
    stopAdapter();
    EXPECT_FALSE(adapter_->failed());
}

TEST_F(SimNowAdapter, DisablesAndRestoresTradingWhenQuotesBecomeInvalidAndRecover) {
    startAdapter();
    firstDepthSnapshot();
    auto invalid = fake_->depth();
    invalid.AskVolume1 = 0;
    fake_->emit(Kind::Depth, invalid);
    waitUntil([&] { return !ready_.load(); }, [&] { return "ready=" + std::to_string(ready_.load()); });
    send();
    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, response().type_);
    EXPECT_EQ(0U, fake_->count());
    fake_->emit(Kind::Depth, fake_->depth());
    waitUntil([&] { return ready_.load(); }, [&] { return "ready=" + std::to_string(ready_.load()); });
    send(2);
    awaitFirstInsert();
    stopAdapter();
    EXPECT_FALSE(adapter_->failed());
}

TEST_F(SimNowAdapter, RejectsOrdersAndDisablesTradingAfterAMarketDisconnect) {
    startAdapter();
    fake_->market({Kind::Disconnected, 0, 4097, true, {}});
    awaitFailure();
    waitUntil([&] { return !ready_.load(); }, [&] { return "ready=" + std::to_string(ready_.load()); });
    send();
    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, response().type_);
    EXPECT_EQ(0U, fake_->count());
    stopAdapter();
}

TEST_F(SimNowAdapter, RefusesStartupWhenMarketAndTradingDaysDisagree) {
    arrange([](Fake& fake) { fake.market_day = "20260929"; });
    EXPECT_THROW(startAdapter(), std::runtime_error);
    EXPECT_TRUE(adapter_->failed());
    EXPECT_FALSE(ready_.load());
}

TEST_F(SimNowAdapter, TimesOutOnAMarketLoginFromAnotherSession) {
    arrange([](Fake& fake) { fake.market_login_id = 999; });

    EXPECT_THROW(startAdapter(), std::runtime_error);

    EXPECT_EQ(0, fake_->subscriptions.load());
    EXPECT_TRUE(adapter_->failed());
}

TEST_F(SimNowAdapter, RefusesToStartWhenThePositionIsDirty) {
    arrange([](Fake& fake) { fake.dirty = true; });

    EXPECT_THROW(startAdapter(), std::runtime_error);

    EXPECT_TRUE(adapter_->failed());
    EXPECT_FALSE(ready_.load());
}

TEST_F(SimNowAdapter, RefusesToStartWhileOrdersAreWorking) {
    arrange([](Fake& fake) { fake.working = true; });

    EXPECT_THROW(startAdapter(), std::runtime_error);

    EXPECT_TRUE(adapter_->failed());
    EXPECT_FALSE(ready_.load());
}

TEST_F(SimNowAdapter, RefusesToStartWhenTheInstrumentIsMissing) {
    arrange([](Fake& fake) { fake.no_instrument = true; });

    EXPECT_THROW(startAdapter(), std::runtime_error);

    EXPECT_TRUE(adapter_->failed());
    EXPECT_FALSE(ready_.load());
}

TEST_F(SimNowAdapter, RetriesAStartupQueryAfterAThrottleRejection) {
    arrange([](Fake& fake) { fake.query_throttle = true; });
    startAdapter();

    EXPECT_TRUE(ready_.load());
    EXPECT_EQ(2, fake_->query_count.load());

    const auto snapshot = firstDepthSnapshot();

    EXPECT_EQ(Ctp::price(800), snapshot.bids_[0].price_);
    EXPECT_EQ(Common::Price_INVALID, snapshot.bids_[1].price_);

    stopAdapter();

    EXPECT_FALSE(adapter_->failed());
}

TEST_F(SimNowAdapter, ThrowsWhenTheEventQueueOverflowsBeforeStart) {
    arrange([](Fake& fake) { fake.flood = true; });

    try {
        startAdapter();
        ADD_FAILURE() << "expected queue exhaustion to abort the startup";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string::npos, std::string(error.what()).find("queue exhausted"))
                << "observed message: " << error.what();
    }

    EXPECT_TRUE(adapter_->failed());
    EXPECT_FALSE(ready_.load());
}

TEST_F(SimNowAdapter, SendsTheFirstOrderWithASequentialRefAndOpenOffset) {
    startAdapter();
    send();
    awaitFirstInsert();

    const auto inserted = fake_->latest();

    EXPECT_EQ("21", std::string(inserted.OrderRef));
    EXPECT_EQ(THOST_FTDC_OF_Open, inserted.CombOffsetFlag[0]);
    EXPECT_EQ(0u, responses_.size());  // SDK return zero is not acceptance.
}

TEST_F(SimNowAdapter, PublishesOneAcceptanceForDuplicateOrderStatusEvents) {
    startAdapter();
    send();
    awaitFirstInsert();

    const auto accepted = fake_->order(fake_->latest(), THOST_FTDC_OST_NoTradeQueueing);
    fake_->emit(Kind::Order, accepted);
    fake_->emit(Kind::Order, accepted);

    EXPECT_EQ(Exchange::ClientResponseType::ACCEPTED, response().type_);
}

TEST_F(SimNowAdapter, RejectsANewOrderWhileAnotherIsOutstanding) {
    startAdapter();
    send();
    awaitFirstInsert();

    fake_->emit(Kind::Order, fake_->order(fake_->latest(), THOST_FTDC_OST_NoTradeQueueing));
    EXPECT_EQ(Exchange::ClientResponseType::ACCEPTED, response().type_);

    send(2);

    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, response().type_);
    EXPECT_EQ(1u, fake_->count());
}

TEST_F(SimNowAdapter, RejectsAnOpenUntilTheExecutionStatusIsKnown) {
    startAdapter();
    send();
    awaitFirstInsert();

    const auto inserted = fake_->latest();
    fake_->emit(Kind::Order, fake_->order(inserted, THOST_FTDC_OST_NoTradeQueueing));
    EXPECT_EQ(Exchange::ClientResponseType::ACCEPTED, response().type_);

    fake_->emit(Kind::Order, fake_->order(inserted, THOST_FTDC_OST_AllTraded, 1));
    send(3);
    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, response().type_);

    const auto trade = fake_->trade(inserted);
    fake_->emit(Kind::Trade, trade);
    fake_->emit(Kind::Trade, trade);

    EXPECT_EQ(1, response().exec_qty_);
    EXPECT_EQ(1, adapter_->position());
}

TEST_F(SimNowAdapter, RejectsAnOpenWhileAPositionIsHeld) {
    startAdapter();
    send();
    awaitFirstInsert();

    const auto inserted = fake_->latest();
    fake_->emit(Kind::Order, fake_->order(inserted, THOST_FTDC_OST_NoTradeQueueing));
    EXPECT_EQ(Exchange::ClientResponseType::ACCEPTED, response().type_);
    fake_->emit(Kind::Trade, fake_->trade(inserted));
    EXPECT_EQ(1, response().exec_qty_);

    send(4);

    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, response().type_);
}

TEST_F(SimNowAdapter, ClosesThePositionWithACloseTodayOrder) {
    startAdapter();
    send();
    awaitFirstInsert();

    const auto inserted = fake_->latest();
    fake_->emit(Kind::Order, fake_->order(inserted, THOST_FTDC_OST_NoTradeQueueing));
    EXPECT_EQ(Exchange::ClientResponseType::ACCEPTED, response().type_);
    fake_->emit(Kind::Trade, fake_->trade(inserted));
    EXPECT_EQ(1, response().exec_qty_);

    send(5, Common::OrderOffset::CLOSE_TODAY, Common::Side::SELL, 800);
    waitUntil([&] { return fake_->count() == 2; },
              [&] { return "inserts=" + std::to_string(fake_->count()); });

    const auto close = fake_->latest();

    EXPECT_EQ(THOST_FTDC_OF_CloseToday, close.CombOffsetFlag[0]);

    fake_->emit(Kind::Trade, fake_->trade(close, "trade2"));
    fake_->emit(Kind::Order, fake_->order(close, THOST_FTDC_OST_AllTraded, 1));

    EXPECT_EQ(Exchange::ClientResponseType::FILLED, response().type_);
    EXPECT_EQ(0, adapter_->position());

    stopAdapter();

    EXPECT_FALSE(adapter_->failed());
}

TEST_F(SimNowAdapter, CancelsTheLiveOrderAndReportsCanceledAtShutdown) {
    startAdapter();
    send();
    awaitFirstInsert();

    stopAdapter();

    EXPECT_EQ(1u, fake_->canceled.size());
    EXPECT_EQ(0, adapter_->position());
    EXPECT_FALSE(adapter_->failed());
    EXPECT_EQ(Exchange::ClientResponseType::CANCELED, response().type_);
}

TEST_F(SimNowAdapter, ReportsACancelFilledAtShutdownAsAnExecution) {
    arrange([](Fake& fake) { fake.cancel_fill = true; });
    startAdapter();
    send();
    awaitFirstInsert();

    stopAdapter();

    EXPECT_EQ(1u, fake_->canceled.size());
    EXPECT_EQ(1, adapter_->position());
    EXPECT_FALSE(adapter_->failed());
    EXPECT_EQ(Exchange::ClientResponseType::FILLED, response().type_);
}

TEST_F(SimNowAdapter, FailsWhenShutdownCannotConfirmTheCancel) {
    arrange([](Fake& fake) { fake.no_cancel = true; });
    startAdapter();
    send();
    awaitFirstInsert();

    stopAdapter();

    EXPECT_EQ(1u, fake_->canceled.size());
    EXPECT_TRUE(adapter_->failed());
    EXPECT_EQ(0u, responses_.size());
}

TEST_F(SimNowAdapter, FailsAndRejectsRequestsAfterADisconnect) {
    startAdapter();
    const VenueAdapter& venue = *adapter_;
    EXPECT_FALSE(venue.failed());
    EXPECT_TRUE(venue.failure().empty());

    fake_->emit(Kind::Disconnected, {}, 0, 4097);
    awaitFailure();

    send();

    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, response().type_);
    EXPECT_EQ(0u, fake_->count());

    const auto failure = venue.failure();
    EXPECT_EQ("CTP disconnected; execution status may be unknown", failure);
    stopAdapter();
    EXPECT_FALSE(venue.running());
    EXPECT_TRUE(venue.failed());
    EXPECT_EQ(failure, venue.failure());
}

TEST_F(SimNowAdapter, FailsWhenTheTradingDayChangesUnderneath) {
    startAdapter();

    auto depth = fake_->depth();
    text(depth.TradingDay, "20260929");
    fake_->emit(Kind::Depth, depth);
    awaitFailure();

    stopAdapter();
}

TEST_F(SimNowAdapter, ReportsAnInsertSdkFailureAsARejection) {
    arrange([](Fake& fake) { fake.reject_insert = true; });
    startAdapter();
    send();

    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, response().type_);
    EXPECT_EQ(0, adapter_->position());

    stopAdapter();

    EXPECT_FALSE(adapter_->failed());
}

TEST_F(SimNowAdapter, RejectsAPriceOffTheInstrumentTick) {
    startAdapter();

    send(1, Common::OrderOffset::OPEN, Common::Side::BUY, 800.01);

    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, response().type_);
    EXPECT_EQ(0u, fake_->count());
}

TEST_F(SimNowAdapter, ReportsAnInsertErrorEventAsARejection) {
    startAdapter();
    send();
    awaitFirstInsert();

    const auto inserted = fake_->latest();
    fake_->emit(Kind::InsertError, inserted, 0, 31);
    fake_->emit(Kind::InsertError, inserted, 0, 31);

    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, response().type_);

    stopAdapter();

    EXPECT_FALSE(adapter_->failed());
    EXPECT_EQ(0u, responses_.size());
}

TEST_F(SimNowAdapter, ReportsAnInsertRejectedSubmitStatusAsARejection) {
    startAdapter();
    send();
    awaitFirstInsert();

    auto rejected = fake_->order(fake_->latest(), THOST_FTDC_OST_Canceled);
    rejected.OrderSubmitStatus = THOST_FTDC_OSS_InsertRejected;
    fake_->emit(Kind::Order, rejected);

    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, response().type_);

    stopAdapter();

    EXPECT_FALSE(adapter_->failed());
    EXPECT_EQ(0u, responses_.size());
}

class SimNowVenueEngine : public ::testing::Test {
  protected:
    SimNowVenueEngine() : engine_(1, {1, 1, 0}, &requests_, &responses_, &updates_, &decisions_) {
        engine_.enableSingleOrderVenue();
        engine_.updateVenueState(true, {900, 100, 1000}, 1000);
    }

    void publishBook(double bid, double ask) {
        Exchange::MarketUpdate update;
        update.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
        update.ticker_id_ = 0;
        update.depth_snapshot_.bids_[0] = {Ctp::price(bid), 10};
        update.depth_snapshot_.asks_[0] = {Ctp::price(ask), 10};
        *updates_.tryGetNextToWriteTo() = update;
        updates_.updateWriteIndex();
        engine_.processPending();
    }

    void decide(std::uint64_t id, JevBias bias, JevIntent intent) {
        engine_.registerEvaluation(0, id);
        JevDecision decision;
        decision.ticker_id_ = 0;
        decision.evaluation_id_ = id;
        decision.bias_ = bias;
        decision.intent_ = intent;
        *decisions_.tryGetNextToWriteTo() = decision;
        decisions_.updateWriteIndex();
        engine_.processPending();
    }

    Exchange::ClientRequest acceptFirstRequest() {
        auto request = *requests_.getNextToRead();
        requests_.updateReadIndex();
        Exchange::ClientResponse response;
        response.type_ = Exchange::ClientResponseType::ACCEPTED;
        response.ticker_id_ = 0;
        response.client_order_id_ = request.order_id_;
        response.side_ = Common::Side::BUY;
        *responses_.tryGetNextToWriteTo() = response;
        responses_.updateWriteIndex();
        engine_.processPending();
        return request;
    }

    void fillFirstRequest(Common::OrderId order_id) {
        Exchange::ClientResponse response;
        response.type_ = Exchange::ClientResponseType::FILLED;
        response.ticker_id_ = 0;
        response.client_order_id_ = order_id;
        response.exec_qty_ = 1;
        response.leaves_qty_ = 0;
        response.price_ = Ctp::price(800.02);
        response.offset_ = Common::OrderOffset::OPEN;
        response.side_ = Common::Side::BUY;
        *responses_.tryGetNextToWriteTo() = response;
        responses_.updateWriteIndex();
        engine_.processPending();
    }

    void openAndFillTheVenueOrder() {
        publishBook(800, 800.02);
        decide(1, JevBias::LONG, JevIntent::OPEN);
        const auto request = acceptFirstRequest();
        fillFirstRequest(request.order_id_);
    }

    Exchange::ClientRequestLFQueue requests_{32};
    Exchange::ClientResponseLFQueue responses_{32};
    Exchange::MarketUpdateLFQueue updates_{32};
    JevDecisionLFQueue decisions_{32};
    TradeEngine engine_;
};

TEST_F(SimNowVenueEngine, ForwardsAnOpenDecisionToTheRequestQueue) {
    publishBook(800, 800.02);

    decide(1, JevBias::LONG, JevIntent::OPEN);

    EXPECT_EQ(1u, requests_.size());
}

TEST_F(SimNowVenueEngine, HoldsTheVenueOrderInsteadOfRepricing) {
    openAndFillTheVenueOrder();
    publishBook(800, 800.04);

    decide(2, JevBias::LONG, JevIntent::OPEN);
    decide(3, JevBias::SHORT, JevIntent::OPEN);

    EXPECT_EQ(0u, requests_.size());
}

TEST_F(SimNowVenueEngine, BlocksNewOpensWhileThePositionIsHeld) {
    openAndFillTheVenueOrder();

    decide(4, JevBias::LONG, JevIntent::OPEN);

    EXPECT_EQ(0u, requests_.size());
}

TEST_F(SimNowVenueEngine, ReportsUnrealizedPnlAndOmitsDepthFromTheEvaluationState) {
    openAndFillTheVenueOrder();
    publishBook(800.04, 800.06);
    decide(4, JevBias::LONG, JevIntent::OPEN);

    const auto state = engine_.buildJevEvaluationState(0, 4);
    JevHttpClient client({});
    const auto json = nlohmann::json::parse(client.buildRequestBody(state));

    EXPECT_NEAR(30.0, state.position_.unrealized_pnl_, 0.001);
    EXPECT_DOUBLE_EQ(800.02, json["state"]["position"]["average_entry_price"].get<double>());
    EXPECT_FALSE(json["state"]["depth"].contains("bids"));
    EXPECT_FALSE(json["state"]["depth"].contains("asks"));
    EXPECT_NEAR(800.05, json["state"]["features"]["mid_price"].get<double>(), 1e-9);
    EXPECT_TRUE(json["state"]["position"].contains("pnl_basis"));
}

TEST_F(SimNowVenueEngine, RefreshesTheAccountAndMultiplierInLaterEvaluations) {
    openAndFillTheVenueOrder();
    publishBook(800.04, 800.06);

    engine_.updateVenueState(true, {500, 500, 1000}, 2000);
    const auto refreshed = engine_.buildJevEvaluationState(0, 5);

    EXPECT_EQ(500, refreshed.account_.available_margin_);
    EXPECT_NEAR(60.0, refreshed.position_.unrealized_pnl_, 0.001);
}

TEST_F(SimNowVenueEngine, DropsDecisionsWhenVenueTradingIsDisabled) {
    openAndFillTheVenueOrder();

    engine_.disableVenueTrading();
    decide(5, JevBias::LONG, JevIntent::CLOSE);

    EXPECT_EQ(0u, requests_.size());
}

struct PriceCase {
    const char* label;
    double input;
};

constexpr PriceCase kPriceCases[] = {
        {"MaxDouble", std::numeric_limits<double>::max()},
        {"QuietNan", std::numeric_limits<double>::quiet_NaN()},
};

class CtpPriceConversion : public ::testing::TestWithParam<PriceCase> {};

TEST_P(CtpPriceConversion, MapsUnconvertibleInputsToInvalid) {
    EXPECT_EQ(Common::Price_INVALID, Ctp::price(GetParam().input));
}

INSTANTIATE_TEST_SUITE_P(Unconvertible, CtpPriceConversion,
                         ::testing::ValuesIn(kPriceCases),
                         [](const ::testing::TestParamInfo<PriceCase>& info) {
                             return info.param.label;
                         });

}  // namespace
