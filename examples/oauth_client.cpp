#include <mcp/all.h>
#include <iostream>

int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      std::cerr << "Usage: mcp_oauth_client <server-url> <registered-redirect-uri>\n";
      return 2;
    }
    mcp::OAuthTransportOptions options;
    options.http.url = argv[1];
    options.oauth.redirect_uri = argv[2];
    options.authorize = [](const mcp::AuthorizationSession& session, const mcp::RequestOptions&) {
      std::cerr << "Authorize in your browser:\n" << session.url << "\n";
      std::cerr << "Enter the actual callback parameters as JSON (code, state and iss when present):\n";
      std::string callback;
      if (!std::getline(std::cin, callback))
        throw std::runtime_error("Authorization cancelled");
      return mcp::parse_json(callback);
    };
    mcp::Client client(std::make_shared<mcp::OAuthTransport>(std::move(options)));
    client.discover();
    std::cout << mcp::serialize_json(client.list_tools()) << '\n';
    client.close();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
