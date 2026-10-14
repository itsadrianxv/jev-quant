#include "simnow_config.h"
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace Trading {
namespace {
std::string trim(const std::string& value) {
    const auto first=value.find_first_not_of(" \t\r\n");
    if(first==std::string::npos) return {};
    return value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
}
std::string credential(const char* name,const std::string& path) {
    const auto* value=std::getenv(name);
    if(value && *value) return value;
    std::ifstream file(path);
    std::string line;
    while(std::getline(file,line)) {
        if(line.starts_with("\xEF\xBB\xBF")) line.erase(0,3);
        line=trim(line);
        if(line.empty() || line.front()=='#') continue;
        if(line.starts_with("export ")) line=trim(line.substr(7));
        const auto equals=line.find('=');
        if(equals==std::string::npos || trim(line.substr(0,equals))!=name) continue;
        auto result=trim(line.substr(equals+1));
        if(!result.empty() && (result.front()=='\'' || result.front()=='"')) {
            const auto end=result.find(result.front(),1);
            if(end==std::string::npos ||
               (!trim(result.substr(end+1)).empty() && trim(result.substr(end+1)).front()!='#'))
                throw std::runtime_error(std::string("Invalid dotenv value for ")+name);
            result=result.substr(1,end-1);
        } else {
            const auto comment=result.find(" #");
            if(comment!=std::string::npos) result=trim(result.substr(0,comment));
        }
        if(!result.empty()) return result;
        break;
    }
    throw std::runtime_error(std::string("Missing credential in environment or dotenv: ")+name);
}
}

SimNowConfig SimNowConfig::fromJson(const nlohmann::json& j,Common::TickerId ticker,
                                  const std::string& dotenv_path) {
    SimNowConfig c;
    c.trading_front=j.at("trading_front").get<std::string>();
    c.market_front=j.at("market_front").get<std::string>();
    c.instrument=j.at("instrument").get<std::string>();
    c.user=credential("SIMNOW_USER_ID",dotenv_path);
    c.password=credential("SIMNOW_PASSWORD",dotenv_path);
    c.auth_code="0000000000000000";
    c.ticker_id=ticker;
    c.flow_directory=j.value("flow_directory",std::string("flows/simnow"));
    c.startup_timeout=std::chrono::milliseconds(j.value("startup_timeout_ms",60000));
    c.shutdown_timeout=std::chrono::milliseconds(j.value("shutdown_timeout_ms",10000));
    return c;
}

}  // namespace Trading
