#include <mcp/all.h>
#include <iostream>
#include <memory>
using namespace mcp;
class LocalTransport final : public Transport {
 public:
  explicit LocalTransport(Server& server) : server_(server) {}
  Json exchange(const Json& request, const RequestOptions& options, NotificationHandler notify) override {
    return server_.dispatch(request, options.stop, std::move(notify));
  }
  void close() noexcept override {}
 private:
  Server& server_;
};
int main() {
  Server server;
  server.add_tool({"echo", "Installed package roundtrip", {{"type", "object"}}},
    [](const Json& args, RequestContext&) { return text_result(args.at("message").get<std::string>()); });
  Client client(std::make_shared<LocalTransport>(server));
  const auto result = client.call_tool("echo", {{"message", "installed MCP consumer passed"}});
  std::cout << serialize_json(result) << '\n';
  return result.at("content").at(0).at("text") == "installed MCP consumer passed" ? 0 : 1;
}
