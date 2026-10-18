#pragma once
#include "private_files.hpp"
#include <functional>
#include <nlohmann/json.hpp>
namespace rimes::runtime {
// Private bearer token. Reading a settings preview never calls Current().
// Rotate invalidates old requests under the same lock used by publication.
class GatewayAccess {
  public:
    explicit GatewayAccess(std::filesystem::path root = UserRoot() / "gateway");
    std::string Current();
    std::string Rotate();
    bool Matches(const std::string& candidate) const;
    void WithBearer(const std::string& candidate, const std::function<void()>& action) const;

  private:
    PrivateDirectory files_;
};
const nlohmann::json& GatewayClients();
std::string GatewayConfiguration(const std::string& client, unsigned port,
                                 const std::string& token = "••••••••", bool supplementary = false);
} // namespace rimes::runtime
