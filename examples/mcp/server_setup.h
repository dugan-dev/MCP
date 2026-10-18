#pragma once
#include <mcp/server.h>
#include <thread>

inline void register_examples(mcp::Server &server) {
    server.add_tool({"echo",
                     "Return the supplied message",
                     {{"type", "object"},
                      {"properties", {{"message", {{"type", "string"}}}}},
                      {"required", {"message"}},
                      {"additionalProperties", false}}},
                    [](const mcp::Json &args, mcp::RequestContext &) {
                        return mcp::text_result(mcp::text(args["message"]));
                    });
    server.add_tool({"add",
                     "Add two numbers",
                     {{"type", "object"},
                      {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}},
                      {"required", {"a", "b"}}},
                     mcp::Json{{"type", "number"}}},
                    [](const mcp::Json &args, mcp::RequestContext &) {
                        auto result = mcp::text_result("Sum");
                        result["structuredContent"] =
                            args["a"].get<double>() + args["b"].get<double>();
                        return result;
                    });
    server.add_tool({"slow", "Demonstrate cooperative cancellation", {{"type", "object"}}},
                    [](const mcp::Json &, mcp::RequestContext &context) {
                        for (int i = 0; i < 1000; ++i) {
                            context.throw_if_cancelled();
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        }
                        return mcp::text_result("Done");
                    });
    server.add_resource({"example://hello", "hello", "Greeting", "text/plain"},
                        [](const mcp::Json &, mcp::RequestContext &) {
                            return mcp::text_resource("example://hello", "Hello, MCP!");
                        });
    server.add_prompt({"greet", "Generate a greeting",
                       mcp::Json::array({{{"name", "name"}, {"required", true}}})},
                      [](const mcp::Json &args, mcp::RequestContext &) {
                          return mcp::prompt_result("Greet " +
                                                    std::string(mcp::text(args["name"])));
                      });
}
