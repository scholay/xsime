#include "configuration.hpp"
#include "gateway_access.hpp"
#include <algorithm>
#include <iostream>
using namespace rimes::runtime;
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--status") {
            const auto value = PrivateDirectory(UserRoot() / "gateway").Read("status.json", 4096);
            auto status = value ? Json::parse(*value) : Json{{"state", "not-started"}};
            if (value) {
                try {
                    auto unowned = PrivateDirectory(UserRoot() / "gateway/host").Lock(false);
                    status["state"] = "stopped";
                } catch (const StorageBusy&) {
                    // A live owner holds this profile; report its latest non-secret state.
                }
            }
            std::cout << status.dump() << '\n';
            return 0;
        }
        if ((argc == 2 && std::string(argv[1]) == "--rotate-token") ||
            (argc == 3 && (std::string(argv[1]) == "--block-source" ||
                           std::string(argv[1]) == "--unblock-source"))) {
            ConfigurationStore store(UserRoot() / "configuration", false);
            auto next = store.Load();
            const std::string action = argv[1];
            if (action == "--rotate-token") {
                (void)GatewayAccess().Rotate();
                // Publishing a fresh settings revision also discards partial streams.
                store.Save(next, next.at("revision"));
                std::cout << "网关授权已更新，请在设置中重新复制配置。\n";
            } else {
                const std::string source = argv[2];
                auto& values = next.at("gatewayBlockedSources");
                values.erase(std::remove(values.begin(), values.end(), source), values.end());
                if (action == "--block-source")
                    values.push_back(source);
                store.Save(next, next.at("revision"));
                std::cout << "来源规则已更新。\n";
            }
            return 0;
        }
        std::cerr << "Usage: rimes-gateway --status | --rotate-token | --block-source KIND:NAME | "
                     "--unblock-source KIND:NAME\n";
        return 2;
    } catch (const std::exception&) {
        std::cerr << "网关操作未完成，请检查私有目录权限或稍后重试。\n";
        return 1;
    }
}
