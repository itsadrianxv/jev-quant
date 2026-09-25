#include <iostream>

#include "trading/strategy/jev_config.h"
#include "trading/strategy/jev_http_client.h"

int main(int argc, char** argv) {
  const auto dotenv_path = argc > 1 ? argv[1] : ".env";
  const auto json_path = argc > 2 ? argv[2] : "config.json";
  const auto config = Trading::loadJevHttpConfig(dotenv_path, json_path);
  Trading::JevHttpClient client(config);
  Trading::JevEvaluationState state;
  state.evaluation_id_ = 1;
  state.ticker_id_ = 0;
  const auto decision = client.evaluate(state);
  if (decision.evaluation_id_ != state.evaluation_id_ ||
      decision.ticker_id_ != state.ticker_id_) {
    std::cerr << "live Jev response identity mismatch\n";
    return 1;
  }
  std::cout << "live Jev smoke passed\n";
  return 0;
}
