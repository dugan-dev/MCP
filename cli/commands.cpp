#include "../src/process_launch.h"
#include <mcp/mcp.h>
namespace bp = boost::process::v2;
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using mcp::Json;
volatile std::sig_atomic_t stopped = 0;
void stop_handler(int) {
  stopped = 1;
}

struct Failure : std::runtime_error {
  int status;
  Failure(int code, std::string message) : std::runtime_error(std::move(message)), status(code) {}
};

[[noreturn]] void usage_error(std::string message) {
  throw Failure(2, std::move(message));
}
void help(std::ostream& out) {
  out << "MCP command line client\n\n"
         "Usage:\n"
         "  mcp --url URL [options] discover\n"
         "  mcp --url URL [options] tools list|call NAME [JSON]\n"
         "  mcp --url URL [options] resources list|templates|read URI\n"
         "  mcp --url URL [options] prompts list|get NAME [JSON]\n"
         "  mcp --url URL [options] tasks get|cancel ID|update ID JSON\n"
         "  mcp --url URL [options] complete REF JSON\n"
         "  mcp --url URL [options] request METHOD [JSON]\n"
         "  mcp --exec PROGRAM [--arg ARG ...] [--cwd DIR] [--env KEY=VALUE ...] [options] "
         "<client command>\n"
         "  mcp inspect --inspector PROGRAM [--arg ARG ...] [-- INSPECTOR-ARGS ...]\n"
         "  mcp serve --config FILE --transport stdio|http|https [--host HOST] [--port "
         "PORT]\n\n"
         "Options: --header 'Name: value'  --timeout-ms N  --version  --help\n"
         "JSON arguments are parsed strictly. Process arguments are passed directly, without a "
         "shell.\n";
}

Json read_json(const std::string& value, std::string_view label) {
  try {
    return mcp::parse_json(value);
  } catch (const std::exception&) {
    usage_error(std::string(label) + " must be valid JSON");
  }
}
Json load_json(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    usage_error("cannot open config file");
  std::string data((std::istreambuf_iterator<char>(input)), {});
  try {
    return mcp::parse_json(data);
  } catch (const std::exception&) {
    usage_error("config file contains invalid JSON");
  }
}
std::string string_field(const Json& value, std::string_view key, std::string fallback = {}) {
  auto it = value.find(key);
  if (it == value.end())
    return fallback;
  if (!it->is_string())
    usage_error("config field '" + std::string(key) + "' must be a string");
  return it->get<std::string>();
}
std::vector<std::string> string_array_field(const Json& value, std::string_view key) {
  auto it = value.find(key);
  if (it == value.end())
    return {};
  if (!it->is_array())
    usage_error("config field '" + std::string(key) + "' must be an array of strings");
  std::vector<std::string> result;
  result.reserve(it->size());
  for (const auto& entry : *it) {
    if (!entry.is_string())
      usage_error("config field '" + std::string(key) + "' must contain only strings");
    result.push_back(entry.get<std::string>());
  }
  return result;
}
bool loopback_host(std::string_view host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}
void print_json(const Json& value) {
  std::cout << mcp::serialize_json(value) << '\n';
}

struct Options {
  std::string url;
  std::string executable;
  std::vector<std::string> arguments;
  mcp::Headers headers;
  std::chrono::milliseconds timeout{30000};
  std::string working_directory;
  std::map<std::string, std::string, std::less<>> environment;
  std::vector<std::string> command;
};

Options parse_options(int argc, char** argv, int begin) {
  Options options;
  int i = begin;
  for (; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--") {
      ++i;
      break;
    }
    if (arg.empty() || arg[0] != '-')
      break;
    if (arg == "--url" || arg == "--exec" || arg == "--arg" || arg == "--header" || arg == "--timeout-ms" || arg == "--cwd" || arg == "--env") {
      if (++i >= argc)
        usage_error("missing value for " + arg);
      std::string value = argv[i];
      if (arg == "--url")
        options.url = value;
      else if (arg == "--exec")
        options.executable = value;
      else if (arg == "--arg")
        options.arguments.push_back(value);
      else if (arg == "--cwd")
        options.working_directory = value;
      else if (arg == "--env") {
        const auto equal = value.find('=');
        if (equal == std::string::npos || equal == 0)
          usage_error("--env expects KEY=VALUE");
        options.environment[value.substr(0, equal)] = value.substr(equal + 1);
      } else if (arg == "--header") {
        auto colon = value.find(':');
        if (colon == std::string::npos || colon == 0)
          usage_error("--header expects 'Name: value'");
        auto name = value.substr(0, colon), content = value.substr(colon + 1);
        auto first = content.find_first_not_of(" \t");
        options.headers[name] = first == std::string::npos ? "" : content.substr(first);
      } else {
        try {
          auto ms = std::stoll(value);
          if (ms < 1 || ms > 86400000)
            throw std::out_of_range("timeout");
          options.timeout = std::chrono::milliseconds(ms);
        } catch (...) {
          usage_error("--timeout-ms must be between 1 and 86400000");
        }
      }
    } else
      usage_error("unknown option: " + arg);
  }
  for (; i < argc; ++i)
    options.command.emplace_back(argv[i]);
  if (options.url.empty() == options.executable.empty())
    usage_error("choose exactly one of --url or --exec");
  if (!options.url.empty() && (!options.arguments.empty() || !options.working_directory.empty() || !options.environment.empty()))
    usage_error("--arg, --cwd, and --env require --exec");
  if (!options.executable.empty() && !options.headers.empty())
    usage_error("--header requires --url");
  return options;
}

Json client_command(mcp::Client& client, const std::vector<std::string>& args, mcp::RequestOptions req) {
  if (args.empty())
    usage_error("missing client command");
  const auto& verb = args[0];
  if (verb == "discover" && args.size() == 1)
    return client.discover(req);
  if (verb == "tools" && args.size() >= 2) {
    if (args[1] == "list" && args.size() == 2)
      return client.list_tools(req);
    if (args[1] == "call" && args.size() >= 3 && args.size() <= 4)
      return client.call_tool(args[2], args.size() == 4 ? read_json(args[3], "tool arguments") : Json::object(), req);
  }
  if (verb == "resources" && args.size() == 3 && args[1] == "read")
    return client.read_resource(args[2], req);
  if (verb == "resources" && args.size() == 2) {
    if (args[1] == "list")
      return client.list_resources(req);
    if (args[1] == "templates")
      return client.list_resource_templates(req);
  }
  if (verb == "prompts" && args.size() >= 2) {
    if (args[1] == "list" && args.size() == 2)
      return client.list_prompts(req);
    if (args[1] == "get" && args.size() >= 3 && args.size() <= 4)
      return client.get_prompt(args[2], args.size() == 4 ? read_json(args[3], "prompt arguments") : Json::object(), req);
  }
  if (verb == "complete" && args.size() == 3)
    return client.complete(read_json(args[1], "completion reference"), read_json(args[2], "completion argument"), req);
  if (verb == "tasks" && args.size() >= 3) {
    Json params = {{"taskId", args[2]}};
    if (args[1] == "get" && args.size() == 3)
      return client.request("tasks/get", std::move(params), req);
    if (args[1] == "cancel" && args.size() == 3)
      return client.request("tasks/cancel", std::move(params), req);
    if (args[1] == "update" && args.size() == 4) {
      auto responses = read_json(args[3], "task input responses");
      if (!responses.is_object())
        usage_error("task input responses must be a JSON object");
      params["inputResponses"] = std::move(responses);
      return client.request("tasks/update", std::move(params), req);
    }
  }
  if (verb == "request" && (args.size() == 2 || args.size() == 3))
    return client.request(args[1], args.size() == 3 ? read_json(args[2], "request params") : Json::object(), req);
  usage_error("invalid command or arguments; run 'mcp --help'");
}

int run_client(int argc, char** argv, int start) {
  auto options = parse_options(argc, argv, start);
  std::shared_ptr<mcp::Transport> transport;
  if (!options.url.empty()) {
    if (options.url.rfind("http://", 0) != 0 && options.url.rfind("https://", 0) != 0)
      usage_error("--url must use http:// or https://");
    mcp::HttpClientOptions http;
    http.url = options.url;
    http.headers = std::move(options.headers);
    transport = std::make_shared<mcp::HttpTransport>(std::move(http));
  } else {
    mcp::ProcessOptions process;
    process.executable = std::move(options.executable);
    process.arguments = std::move(options.arguments);
    process.working_directory = std::move(options.working_directory);
    process.environment = std::move(options.environment);
    transport = std::make_shared<mcp::StdioTransport>(std::move(process));
  }
  mcp::Client client(std::move(transport));
  mcp::RequestOptions request;
  request.timeout = options.timeout;
  print_json(client_command(client, options.command, request));
  return 0;
}

int run_inspect(int argc, char** argv) {
  std::string executable;
  std::vector<std::string> arguments;
  bool forwarded = false;
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (forwarded) {
      arguments.push_back(std::move(arg));
      continue;
    }
    if (arg == "--") {
      forwarded = true;
      continue;
    }
    if (arg == "--inspector" || arg == "--arg") {
      if (++i >= argc)
        usage_error("missing value for " + arg);
      if (arg == "--inspector") {
        if (!executable.empty())
          usage_error("--inspector may be specified only once");
        executable = argv[i];
      } else
        arguments.emplace_back(argv[i]);
      continue;
    }
    usage_error("inspect accepts --inspector PROGRAM, --arg ARG, and -- INSPECTOR-ARGS");
  }
  if (executable.empty())
    usage_error("inspect requires an explicit --inspector PROGRAM");
  mcp::detail::validate_process_arguments(arguments);
  boost::asio::io_context context;
  bp::process child(context, mcp::detail::process_path(executable), arguments, bp::process_stdio{});
  return child.wait();
}

void serve_config(const Json& config) {
  if (!config.is_object())
    usage_error("config root must be an object");
  mcp::ServerOptions server_options;
  server_options.info["name"] = string_field(config, "name", "MCP");
  server_options.info["version"] = string_field(config, "version", std::string(mcp::library_version));
  mcp::Server server(server_options);
  if (auto tools = config.find("tools"); tools != config.end()) {
    if (!tools->is_array())
      usage_error("config 'tools' must be an array");
    for (const auto& entry : *tools) {
      if (!entry.is_object())
        usage_error("each tool config must be an object");
      mcp::Tool tool;
      tool.name = string_field(entry, "name");
      if (tool.name.empty())
        usage_error("configured tool needs a name");
      tool.description = string_field(entry, "description");
      if (auto schema = entry.find("inputSchema"); schema != entry.end())
        tool.input_schema = *schema;
      Json result = entry.value("result", Json::object());
      server.add_tool(std::move(tool), [result = std::move(result)](const Json&, mcp::RequestContext&) { return result; });
    }
  }
  if (auto resources = config.find("resources"); resources != config.end()) {
    if (!resources->is_array())
      usage_error("config 'resources' must be an array");
    for (const auto& entry : *resources) {
      if (!entry.is_object())
        usage_error("each resource config must be an object");
      mcp::Resource resource;
      resource.uri = string_field(entry, "uri");
      resource.name = string_field(entry, "name", resource.uri);
      resource.description = string_field(entry, "description");
      resource.mime_type = string_field(entry, "mimeType", "text/plain");
      if (resource.uri.empty())
        usage_error("configured resource needs a uri");
      Json content = mcp::text_resource(resource.uri, string_field(entry, "text"), resource.mime_type);
      server.add_resource(std::move(resource), [content = std::move(content)](const Json&, mcp::RequestContext&) { return content; });
    }
  }
  if (auto prompts = config.find("prompts"); prompts != config.end()) {
    if (!prompts->is_array())
      usage_error("config 'prompts' must be an array");
    for (const auto& entry : *prompts) {
      if (!entry.is_object())
        usage_error("each prompt config must be an object");
      mcp::Prompt prompt;
      prompt.name = string_field(entry, "name");
      if (prompt.name.empty())
        usage_error("configured prompt needs a name");
      prompt.description = string_field(entry, "description");
      if (auto arguments = entry.find("arguments"); arguments != entry.end())
        prompt.arguments = *arguments;
      Json result = mcp::prompt_result(string_field(entry, "text"));
      server.add_prompt(std::move(prompt), [result = std::move(result)](const Json&, mcp::RequestContext&) { return result; });
    }
  }
  auto transport = string_field(config, "transport", "stdio");
  if (transport == "stdio") {
    mcp::run_stdio(server, std::cin, std::cout);
  } else if (transport == "http" || transport == "https") {
    mcp::HttpServerOptions http;
    http.host = string_field(config, "host", "127.0.0.1");
    if (!loopback_host(http.host))
      usage_error(
          "static serve binds to loopback only; use an authenticated gateway for "
          "remote access");
    http.allowed_hosts = string_array_field(config, "allowedHosts");
    http.allowed_origins = string_array_field(config, "allowedOrigins");
    if (auto port = config.find("port"); port != config.end()) {
      if (!port->is_number_unsigned() && !port->is_number_integer())
        usage_error("config 'port' must be an integer");
      auto value = port->get<long long>();
      if (value < 0 || value > 65535)
        usage_error("config 'port' is out of range");
      http.port = static_cast<std::uint16_t>(value);
    }
    http.path = string_field(config, "path", "/mcp");
    if (transport == "https") {
      http.certificate_file = string_field(config, "certificate");
      http.private_key_file = string_field(config, "privateKey");
      if (http.certificate_file.empty() || http.private_key_file.empty())
        usage_error("https requires certificate and privateKey paths");
    }
    const auto host = http.host;
    const auto path = http.path;
    mcp::HttpServer listener(server, std::move(http));
    listener.start();
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    std::cerr << "MCP server listening on " << (transport == "https" ? "https" : "http") << "://" << host << ':' << listener.port() << path << '\n';
    while (!stopped)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    listener.stop();
  } else
    usage_error("config transport must be stdio, http, or https");
}

int run_serve(int argc, char** argv) {
  std::string path;
  std::string transport;
  Json overrides = Json::object();
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config" || arg == "--transport" || arg == "--host" || arg == "--port") {
      if (++i >= argc)
        usage_error("missing value for " + arg);
      std::string value = argv[i];
      if (arg == "--config")
        path = value;
      else if (arg == "--transport")
        transport = value;
      else if (arg == "--host")
        overrides["host"] = value;
      else {
        try {
          auto p = std::stoll(value);
          if (p < 0 || p > 65535)
            throw std::out_of_range("port");
          overrides["port"] = p;
        } catch (...) {
          usage_error("--port must be between 0 and 65535");
        }
      }
    } else
      usage_error("unknown serve option: " + arg);
  }
  if (path.empty())
    usage_error("serve requires --config FILE");
  Json config = load_json(path);
  for (auto it = overrides.begin(); it != overrides.end(); ++it)
    config[it.key()] = it.value();
  if (!transport.empty())
    config["transport"] = transport;
  serve_config(config);
  return 0;
}

}  // namespace

namespace mcp::cli {
int entry(int argc, char** argv) {
  try {
    if (argc == 1) {
      help(std::cout);
      return 0;
    }
    std::string first = argv[1];
    if (first == "--help" || first == "-h" || first == "help") {
      help(std::cout);
      return 0;
    }
    if (first == "--version" || first == "version") {
      std::cout << "MCP " << mcp::library_version << '\n';
      return 0;
    }
    if (first == "serve")
      return run_serve(argc, argv);
    if (first == "inspect")
      return run_inspect(argc, argv);
    return run_client(argc, argv, 1);
  } catch (const Failure& error) {
    std::cerr << "mcp: " << error.what() << '\n';
    return error.status;
  } catch (const mcp::RpcError& error) {
    std::cerr << "mcp: RPC error " << error.code() << ": " << error.what() << '\n';
    return 4;
  } catch (const mcp::TransportError& error) {
    std::cerr << "mcp: transport error: " << error.what() << '\n';
    return 3;
  } catch (const std::exception& error) {
    std::cerr << "mcp: error: " << error.what() << '\n';
    return 5;
  }
}

}  // namespace mcp::cli
