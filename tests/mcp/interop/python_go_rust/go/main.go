package main

import (
    "context"
    "encoding/json"
    "errors"
    "fmt"
    "net"
    "net/http"
    "os"
    "os/exec"
    "sync"
    "time"

    "github.com/modelcontextprotocol/go-sdk/mcp"
    "github.com/modelcontextprotocol/go-sdk/jsonrpc"
)
const version = "2026-07-28"
func must(err error) { if err != nil { panic(err) } }
func require(ok bool, message string) { if !ok { panic(message) } }
type EchoArgs struct { Message string `json:"message"` }
func makeServer() *mcp.Server {
    server := mcp.NewServer(&mcp.Implementation{Name:"official-go-interop",Version:"1"},
        &mcp.ServerOptions{SupportedProtocolVersions:[]string{version}})
    mcp.AddTool(server,&mcp.Tool{Name:"echo",Description:"echo"},
        func(ctx context.Context, req *mcp.CallToolRequest, args EchoArgs) (*mcp.CallToolResult,any,error) {
            require(req.ProtocolVersion()==version,"legacy request reached Go tool")
            return &mcp.CallToolResult{Content:[]mcp.Content{&mcp.TextContent{Text:args.Message}}},nil,nil
        })
    server.AddResource(&mcp.Resource{URI:"example://hello",Name:"hello",MIMEType:"text/plain"},
        func(ctx context.Context, req *mcp.ReadResourceRequest)(*mcp.ReadResourceResult,error) {
            require(req.ProtocolVersion()==version,"legacy resource request")
            return &mcp.ReadResourceResult{Contents:[]*mcp.ResourceContents{{URI:"example://hello",Text:"Hello, MCP!",MIMEType:"text/plain"}}},nil
        })
    server.AddPrompt(&mcp.Prompt{Name:"greet",Arguments:[]*mcp.PromptArgument{{Name:"name",Required:true}}},
        func(ctx context.Context,req *mcp.GetPromptRequest)(*mcp.GetPromptResult,error) {
            require(req.ProtocolVersion()==version,"legacy prompt request")
            return &mcp.GetPromptResult{Messages:[]*mcp.PromptMessage{{Role:"user",Content:&mcp.TextContent{Text:"Greet "+req.Params.Arguments["name"]}}}},nil
        })
    return server
}
func check(transport mcp.Transport) {
    ctx,cancel:=context.WithTimeout(context.Background(),20*time.Second); defer cancel()
    client:=mcp.NewClient(&mcp.Implementation{Name:"official-go-client",Version:"1"},nil)
    session,err:=client.Connect(ctx,transport,&mcp.ClientSessionOptions{ProtocolVersion:version});must(err);defer session.Close()
    require(session.InitializeResult().ProtocolVersion==version,"did not negotiate modern protocol")
    tools,err:=session.ListTools(ctx,nil);must(err)
    found:=false;for _,tool:=range tools.Tools {found=found||tool.Name=="echo"};require(found,"echo missing")
    var jobs sync.WaitGroup
    failures:=make(chan error,16)
    for i:=0;i<16;i++ { jobs.Add(1);go func(i int){defer jobs.Done()
        message:=fmt.Sprintf("sdk-%d",i)
        result,err:=session.CallTool(ctx,&mcp.CallToolParams{Name:"echo",Arguments:map[string]any{"message":message}})
        if err!=nil {failures<-err;return}
        if result.IsError||len(result.Content)!=1||result.Content[0].(*mcp.TextContent).Text!=message {failures<-fmt.Errorf("echo correlation %d",i)}
    }(i)};jobs.Wait();close(failures);for err:=range failures {must(err)}
    resources,err:=session.ListResources(ctx,nil);must(err);require(len(resources.Resources)>0,"resources missing")
    resource,err:=session.ReadResource(ctx,&mcp.ReadResourceParams{URI:"example://hello"});must(err);require(resource.Contents[0].Text=="Hello, MCP!","resource content")
    prompts,err:=session.ListPrompts(ctx,nil);must(err);require(len(prompts.Prompts)>0,"prompts missing")
    prompt,err:=session.GetPrompt(ctx,&mcp.GetPromptParams{Name:"greet",Arguments:map[string]string{"name":"SDK"}});must(err)
    require(prompt.Messages[0].Content.(*mcp.TextContent).Text=="Greet SDK","prompt content")
    _,err=session.CallTool(ctx,&mcp.CallToolParams{Name:"missing-tool",Arguments:map[string]any{}})
    var wireError *jsonrpc.Error
    require(errors.As(err,&wireError)&&wireError.Code==-32602,"unknown tool did not return invalid_params")
    result,_:=json.Marshal(map[string]any{"sdk":"go","protocol":version,"status":"passed","concurrent_calls":16});fmt.Println(string(result))
}
func main(){
    require(len(os.Args)>=2,"mode required")
    switch os.Args[1] {
    case "server-stdio":must(makeServer().Run(context.Background(),&mcp.StdioTransport{}))
    case "server-http":
        server:=makeServer();listener,err:=net.Listen("tcp","127.0.0.1:0");must(err);fmt.Println(listener.Addr().(*net.TCPAddr).Port)
        handler:=mcp.NewStreamableHTTPHandler(func(*http.Request)*mcp.Server{return server},&mcp.StreamableHTTPOptions{Stateless:true})
        must(http.Serve(listener,handler))
    case "client-stdio":check(&mcp.CommandTransport{Command:exec.Command(os.Args[2],os.Args[3:]...)})
    case "client-http":check(&mcp.StreamableClientTransport{Endpoint:os.Args[2]})
    default:panic("unknown mode")
    }
}
