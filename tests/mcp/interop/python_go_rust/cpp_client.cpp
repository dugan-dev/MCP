#include <mcp/all.h>
#include <future>
#include <iostream>
#include <stdexcept>
void require(bool condition,const char *message){if(!condition)throw std::runtime_error(message);}
int main(int argc,char**argv){try{
    require(argc>=3,"transport and target required");
    std::shared_ptr<mcp::Transport> transport;
    if(std::string_view(argv[1])=="http") {mcp::HttpClientOptions options;options.url=argv[2];transport=std::make_shared<mcp::HttpTransport>(options);}
    else {mcp::ProcessOptions options;options.executable=argv[2];for(int i=3;i<argc;++i)options.arguments.push_back(argv[i]);transport=std::make_shared<mcp::StdioTransport>(options);}
    mcp::Client client(transport);
    require(client.discover()["supportedVersions"].get<std::vector<std::string>>().at(0)=="2026-07-28","modern discovery");
    auto tools=client.list_tools();require(!tools.empty(),"tools missing");
    std::vector<std::future<void>> calls;
    for(int i=0;i<16;++i)calls.push_back(std::async(std::launch::async,[&,i]{
        auto text="sdk-"+std::to_string(i);auto result=client.call_tool("echo",{{"message",text}});
        require(result["content"][0]["text"]==text,"echo correlation");
    }));for(auto &call:calls)call.get();
    require(!client.list_resources().empty(),"resources missing");
    require(client.read_resource("example://hello")["contents"][0]["text"]=="Hello, MCP!","resource content");
    require(!client.list_prompts().empty(),"prompts missing");
    require(client.get_prompt("greet",{{"name","SDK"}})["messages"][0]["content"]["text"]=="Greet SDK","prompt content");
    bool rejected=false;try{client.call_tool("missing-tool",mcp::Json::object());}catch(const mcp::RpcError&error){rejected=error.code()==static_cast<int>(mcp::ErrorCode::invalid_params);}require(rejected,"unknown tool did not return invalid_params");
    client.close();std::cout<<"{\"client\":\"cpp\",\"protocol\":\"2026-07-28\",\"concurrent_calls\":16,\"status\":\"passed\"}\n";
    return 0;
}catch(const std::exception&error){std::cerr<<error.what()<<'\n';return 1;}}
