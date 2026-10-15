#include "harness.h"
#include "trading/performance/measurement.h"
#include "trading/strategy/jev_worker.h"
#include "trading/order_gw/order_gateway.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
TEST(PerformanceMeasurement, ShutdownTimeDeterminesCompletionNotDelayedExport) {
    Trading::Measurement m(1, 100, 50);
    m.startCapture(100);
    m.stopCapture(200);
    const auto directory = std::filesystem::temp_directory_path() /
        ("simnow-measurement-test-" + std::to_string(Common::nowSteadyNs()));
    m.exportTrace(directory.string(), true, 0);
    nlohmann::json metadata;
    std::ifstream(directory / "capture.json") >> metadata;
    EXPECT_FALSE(metadata.at("window_completed").get<bool>());
    EXPECT_EQ(metadata.at("new_samples"), 0);
    EXPECT_THROW(m.exportTrace(directory.string(), true, 0), std::runtime_error);
    std::filesystem::remove_all(directory);
}

TEST(PerformanceMeasurement, BoundsAdmissionGraceSuccessAndOverflow) {
    Trading::Measurement m(1, 100, 50);
    EXPECT_EQ(m.createTrace(100).source_id, 0U);
    m.startCapture(100);
    EXPECT_THROW(m.startCapture(101), std::logic_error);
    EXPECT_FALSE(m.isWithinAdmissionWindow(99));
    EXPECT_TRUE(m.isWithinAdmissionWindow(199));
    EXPECT_FALSE(m.isWithinAdmissionWindow(200));
    EXPECT_TRUE(m.isWithinCollectionWindow(249));
    EXPECT_FALSE(m.isWithinCollectionWindow(250));
    Exchange::ClientRequest r;
    r.type_ = Exchange::ClientRequestType::NEW;
    r.trace_ = m.createTrace(110);
    r.trace_.evaluation_id = 1;
    r.trace_.t = {110,120,130,140,210,220,230,240};
    m.recordTrace(r, -1);
    EXPECT_EQ(m.size(), 0U);
    auto missing = r;
    missing.trace_.t[2] = 0;
    m.recordTrace(missing, 0);
    auto reversed = r;
    reversed.trace_.t[2] = 119;
    m.recordTrace(reversed, 0);
    auto late = r;
    late.trace_.t[7] = 250;
    m.recordTrace(late, 0);
    EXPECT_EQ(m.size(), 0U);
    m.recordTrace(r, 0);
    ASSERT_EQ(m.size(), 1U);
    EXPECT_EQ(m.sample(0).trace.t[0], 110U);
    m.recordTrace(r, 0);
    EXPECT_EQ(m.overflow(), 1U);
    m.stopCapture(245);
    EXPECT_FALSE(m.isWithinCollectionWindow(245));
    EXPECT_TRUE(m.isWithinCollectionWindow(244));
}

TEST(PerformanceMeasurement, CancelsNeedOnlySubmissionBoundaries) {
    Trading::Measurement m(2, 100, 50);
    m.startCapture(100);
    Exchange::ClientRequest r;
    r.type_ = Exchange::ClientRequestType::CANCEL;
    r.trace_.t = {0,0,0,0,190,210,220,230};
    m.recordTrace(r, 0);
    EXPECT_EQ(m.size(), 1U);
    r.trace_.t[4] = 200;
    m.recordTrace(r, 0);
    EXPECT_EQ(m.size(), 1U);
}

class SelectingProvider : public Trading::JevDecisionProvider {
public:
    Trading::JevDecision evaluate(const Trading::JevEvaluationState& state) override {
        selected = state;
        Trading::JevDecision d;
        d.evaluation_id_ = state.evaluation_id_;
        d.ticker_id_ = state.ticker_id_;
        d.bias_ = Trading::JevBias::LONG;
        d.intent_ = Trading::JevIntent::OPEN;
        d.trace_.source_id = 999; // Must never trust provider-supplied correlation.
        return d;
    }
    Trading::JevEvaluationState selected;
};

TEST(PerformanceMeasurement, SelectedEvaluationKeepsItsSourceAcrossNewMarketData) {
    Trading::Measurement m(8, 60000000000ULL, 60000000000ULL);
    m.startCapture();
    auto h = Tests::makeHarness(Tests::EngineHarness::defaultLimits());
    h->engine.setMeasurement(&m);
    auto publish = [&](Common::Price bid) {
        auto* u = h->market_updates.getNextToWriteTo();
        *u = {};
        u->type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
        u->ticker_id_ = 0;
        u->depth_snapshot_.bids_[0] = {bid, 2};
        u->depth_snapshot_.asks_[0] = {bid+1, 3};
        u->trace_ = m.createTrace(Common::nowSteadyNs());
        u->trace_.t[1] = Common::nowSteadyNs();
        u->trace_.t[2] = Common::nowSteadyNs();
        h->market_updates.updateWriteIndex();
        h->engine.processPending();
    };
    Trading::JevEvaluationStateLFQueue evaluations(8);
    SelectingProvider provider;
    Trading::JevWorker worker(&evaluations, &h->decisions, &provider);
    publish(100);
    *evaluations.getNextToWriteTo() = h->engine.buildJevEvaluationState(0,1);
    evaluations.updateWriteIndex();
    publish(102);
    const auto selected = h->engine.buildJevEvaluationState(0,2);
    *evaluations.getNextToWriteTo() = selected;
    evaluations.updateWriteIndex();
    h->engine.registerEvaluation(0,2);
    ASSERT_TRUE(worker.processPending());
    // The new quote is consumed before the selected decision in processPending.
    auto* u = h->market_updates.getNextToWriteTo();
    *u = {};
    u->type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    u->ticker_id_ = 0;
    u->depth_snapshot_.bids_[0] = {104,2};
    u->depth_snapshot_.asks_[0] = {105,3};
    u->trace_ = m.createTrace(Common::nowSteadyNs());
    h->market_updates.updateWriteIndex();
    h->engine.processPending();
    ASSERT_EQ(h->requests.size(), 1U);
    Trading::OrderGateway gateway(7, &h->requests, &h->responses);
    bool called = false;
    gateway.setRequestHandler([&](auto, const auto& request) {
        called = true;
        EXPECT_EQ(request.trace_.source_id, selected.trace_.source_id);
        EXPECT_EQ(request.trace_.evaluation_id, 2U);
        EXPECT_EQ(request.trace_.t[3], selected.trace_.t[3]);
        EXPECT_GE(request.trace_.t[4], selected.trace_.t[3]);
        EXPECT_GE(request.trace_.t[5], request.trace_.t[4]);
        EXPECT_GE(request.trace_.t[6], request.trace_.t[5]);
        // Execution price uses the current book; provenance names the evaluated state.
        EXPECT_EQ(request.price_, 105);
    });
    gateway.processPending();
    EXPECT_TRUE(called);
}

TEST(PerformanceMeasurement, DisabledMeasurementDoesNotStampRequests) {
    auto h = Tests::makeHarness(Tests::EngineHarness::defaultLimits());
    Tests::writeMarketUpdate(h->market_updates,0,100,101);
    h->engine.processPending();
    h->engine.registerEvaluation(0,1);
    Tests::writeDecision(h->decisions,1);
    h->engine.processPending();
    const auto r = Tests::readRequest(h->requests);
    EXPECT_EQ(r.trace_.t[4], 0U);
    EXPECT_EQ(r.trace_.source_id, 0U);
}
}
