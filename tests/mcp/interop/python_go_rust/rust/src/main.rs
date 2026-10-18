use std::borrow::Cow;
use std::io::Write;
use anyhow::{Result, ensure};
use rmcp::{ClientLifecycleMode, ClientServiceExt, ErrorData, RoleServer, ServerHandler, ServiceExt,
    model::*, service::RequestContext,
    transport::{TokioChildProcess, StreamableHttpClientTransport, stdio,
        streamable_http_server::{StreamableHttpService, StreamableHttpServerConfig, session::local::LocalSessionManager}}};
use serde_json::json;

#[derive(Clone)]
struct AuditServer;
fn require_modern(context: &RequestContext<RoleServer>) -> Result<(), ErrorData> {
    if context.protocol_version() != Some(ProtocolVersion::V_2026_07_28) {
        return Err(ErrorData::invalid_params("modern protocol required", None));
    }
    Ok(())
}
impl ServerHandler for AuditServer {
    fn get_info(&self) -> ServerConfig {
        ServerConfig::new(ServerCapabilities::builder().enable_tools().enable_resources().enable_prompts().build())
            .with_server_info(Implementation::new("official-rust-interop", "1"))
            .with_protocol_version(ProtocolVersion::V_2026_07_28)
    }
    fn supported_protocol_versions(&self) -> Cow<'static, [ProtocolVersion]> {
        Cow::Owned(vec![ProtocolVersion::V_2026_07_28])
    }
    async fn list_tools(&self, _: Option<PaginatedRequestParams>, ctx: RequestContext<RoleServer>) -> Result<ListToolsResult, ErrorData> {
        require_modern(&ctx)?;
        Ok(ListToolsResult::with_all_items(vec![Tool::new("echo", "echo",
            json!({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]}).as_object().unwrap().clone())]))
    }
    async fn call_tool(&self, request: CallToolRequestParams, ctx: RequestContext<RoleServer>) -> Result<CallToolResponse, ErrorData> {
        require_modern(&ctx)?;
        if request.name != "echo" { return Err(ErrorData::invalid_params("unknown tool", None)); }
        let message = request.arguments.as_ref().and_then(|a| a.get("message")).and_then(|m| m.as_str())
            .ok_or_else(|| ErrorData::invalid_params("message required", None))?;
        Ok(CallToolResult::success(vec![ContentBlock::text(message)]).into())
    }
    async fn list_resources(&self, _: Option<PaginatedRequestParams>, ctx: RequestContext<RoleServer>) -> Result<ListResourcesResult, ErrorData> {
        require_modern(&ctx)?;
        Ok(ListResourcesResult::with_all_items(vec![Resource::new("example://hello", "hello")]))
    }
    async fn read_resource(&self, request: ReadResourceRequestParams, ctx: RequestContext<RoleServer>) -> Result<ReadResourceResponse, ErrorData> {
        require_modern(&ctx)?;
        if request.uri != "example://hello" { return Err(ErrorData::invalid_params("unknown resource", None)); }
        Ok(ReadResourceResult::new(vec![ResourceContents::text("Hello, MCP!",request.uri)]).into())
    }
    async fn list_prompts(&self, _: Option<PaginatedRequestParams>, ctx: RequestContext<RoleServer>) -> Result<ListPromptsResult, ErrorData> {
        require_modern(&ctx)?;
        Ok(ListPromptsResult::with_all_items(vec![serde_json::from_value(json!({"name":"greet","arguments":[{"name":"name","required":true}]})).unwrap()]))
    }
    async fn get_prompt(&self, request: GetPromptRequestParams, ctx: RequestContext<RoleServer>) -> Result<GetPromptResponse, ErrorData> {
        require_modern(&ctx)?;
        if request.name != "greet" { return Err(ErrorData::invalid_params("unknown prompt", None)); }
        let name = request.arguments.as_ref().and_then(|a| a.get("name")).and_then(|n| n.as_str()).unwrap_or("");
        Ok(GetPromptResult::new(vec![PromptMessage::new(Role::User,ContentBlock::text(format!("Greet {name}")))]).into())
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    let mode = args.get(1).map(String::as_str).unwrap_or("");
    if mode == "server-stdio" {
        AuditServer.serve(stdio()).await?.waiting().await?;
        return Ok(());
    }
    if mode == "server-http" {
        let service = StreamableHttpService::new(|| Ok(AuditServer), LocalSessionManager::default().into(),
            StreamableHttpServerConfig::default().with_legacy_session_mode(false).with_stateless_protocol_metadata_required(true));
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await?;
        println!("{}", listener.local_addr()?.port()); std::io::stdout().flush()?;
        axum::serve(listener,axum::Router::new().nest_service("/mcp",service)).await?;
        return Ok(());
    }
    let config = ClientConfig::new(ClientCapabilities::default(),Implementation::new("official-rust-client","1"));
    let lifecycle = ClientLifecycleMode::Discover { preferred_versions: vec![ProtocolVersion::V_2026_07_28] };
    let client = if mode == "client-http" {
        config.serve_with_lifecycle(StreamableHttpClientTransport::from_uri(args[2].clone()),lifecycle).await?
    } else {
        ensure!(mode == "client-stdio","unknown mode");
        let mut command = tokio::process::Command::new(&args[2]); command.args(&args[3..]);
        config.serve_with_lifecycle(TokioChildProcess::new(command)?,lifecycle).await?
    };
    let discovery = client.discover(Default::default()).await?;
    ensure!(discovery.supported_versions.contains(&ProtocolVersion::V_2026_07_28),"modern discovery failed");
    let tools = client.list_tools(Default::default()).await?;
    ensure!(tools.tools.iter().any(|tool| tool.name == "echo"),"echo missing");
    let calls = (0..16).map(|i| { let peer = client.peer().clone(); async move {
        let message = format!("sdk-{i}");
        let result = peer.call_tool(CallToolRequestParams::new("echo").with_arguments(json!({"message":message}).as_object().unwrap().clone())).await?;
        ensure!(result.content[0].as_text().unwrap().text == message,"echo correlation");
        Ok::<(), anyhow::Error>(())
    }});
    futures::future::try_join_all(calls).await?;
    ensure!(!client.list_resources(Default::default()).await?.resources.is_empty(),"resources missing");
    let resource = serde_json::to_value(client.read_resource(ReadResourceRequestParams::new("example://hello")).await?)?;
    ensure!(resource["contents"][0]["text"] == "Hello, MCP!","resource content");
    ensure!(!client.list_prompts(Default::default()).await?.prompts.is_empty(),"prompts missing");
    let prompt = serde_json::to_value(client.get_prompt(GetPromptRequestParams::new("greet").with_arguments(json!({"name":"SDK"}).as_object().unwrap().clone())).await?)?;
    ensure!(prompt["messages"][0]["content"]["text"] == "Greet SDK","prompt content");
    match client.call_tool(CallToolRequestParams::new("missing-tool")).await {
        Err(rmcp::service::ServiceError::McpError(error)) => ensure!(error.code == ErrorCode::INVALID_PARAMS,"unknown tool error code"),
        _ => anyhow::bail!("unknown tool did not return invalid_params"),
    }
    client.cancel().await?;
    println!("{{\"sdk\":\"rust\",\"protocol\":\"2026-07-28\",\"concurrent_calls\":16,\"status\":\"passed\"}}");
    Ok(())
}
