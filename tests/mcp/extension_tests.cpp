#include <gtest/gtest.h>
#include <mcp/extensions.h>

#include <array>
#include <atomic>
#include <fstream>
#include <openssl/rand.h>
#include <thread>
#include <vector>

using namespace mcp;
using namespace mcp::extensions;

namespace {
struct TempDirectory {
  std::filesystem::path base;
  std::filesystem::path path;
  TempDirectory() {
    std::array<unsigned char, 12> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
      throw std::runtime_error("Unable to generate temporary test directory name");
    static constexpr char hex[] = "0123456789abcdef";
    std::string suffix;
    for (auto byte : bytes) {
      suffix.push_back(hex[byte >> 4]);
      suffix.push_back(hex[byte & 15]);
    }
    base = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
    path = base / ("mcp-extension-test-" + suffix);
    if (path.parent_path() != base)
      throw std::runtime_error("Unsafe temp path");
    if (std::filesystem::exists(path))
      throw std::runtime_error("Temporary test path collision");
    std::filesystem::create_directories(path);
  }
  ~TempDirectory() {
    if (path.parent_path() == base && std::filesystem::weakly_canonical(path).parent_path() == base)
      std::filesystem::remove_all(path);
  }
};
}  // namespace

TEST(TasksExtension, StableCreateAndStatusShapes) {
  auto task = make_task("https://issuer\nuser-7", "job-1", TaskStatus::working, std::chrono::minutes(5), std::chrono::milliseconds(25));
  auto created = create_task_result(task);
  EXPECT_EQ(created["resultType"], "task");
  EXPECT_TRUE(created.contains("taskId"));
  EXPECT_FALSE(created.contains("result"));
  EXPECT_NO_THROW(validate_task(task_result(task)));
  EXPECT_THROW(validate_task(Json{{"resultType", "task"}}, true), RpcError);
}

TEST(TasksExtension, InputResponsesAdvanceAndTerminalStatesAreImmutable) {
  auto task = make_task("issuer\nsubject", "job", TaskStatus::working, std::chrono::minutes(1), std::chrono::milliseconds(10));
  transition_task(task, TaskStatus::input_required,
                  {{"inputRequests",
                    {{"confirm",
                      {{"method", "elicitation/create"},
                       {"params", {{"mode", "form"}, {"message", "Continue?"}, {"requestedSchema", {{"type", "object"}}}}}}}}}});
  accept_task_inputs(task, {{"unknown", {{"action", "accept"}}}});
  EXPECT_EQ(task.wire["status"], "input_required");
  accept_task_inputs(task, {{"confirm", {{"action", "accept"}, {"content", Json::object()}}}});
  EXPECT_EQ(task.wire["status"], "working");
  transition_task(task, TaskStatus::completed, {{"result", {{"content", Json::array()}}}});
  EXPECT_THROW(transition_task(task, TaskStatus::cancelled), std::logic_error);
}

TEST(TasksExtension, StoreScopesOwnersAndUsesCompareExchange) {
  MemoryTaskStore store;
  auto task = store.create(make_task("issuer\nsubject-a", "same-id", TaskStatus::working, std::chrono::minutes(1), std::chrono::milliseconds(5)));
  EXPECT_FALSE(store.get("issuer\nsubject-b", "same-id"));
  EXPECT_FALSE(store.compare_exchange(task.owner, "same-id", 0, task));
  auto next = task;
  request_task_cancel(next);
  EXPECT_TRUE(store.compare_exchange(task.owner, "same-id", task.revision, next));
  EXPECT_EQ(store.get(task.owner, "same-id")->revision, 2);
}

TEST(TasksExtension, MemoryStoreUsesMillisecondTtlAndPreservesDeadlineOnUpdates) {
  MemoryTaskStore store;
  auto task = store.create(make_task("tenant", "short-lived", TaskStatus::working, std::chrono::milliseconds(240), std::chrono::milliseconds(5)));
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  ASSERT_TRUE(store.get("tenant", "short-lived"));
  auto changed = *store.get("tenant", "short-lived");
  request_task_cancel(changed);
  ASSERT_TRUE(store.compare_exchange("tenant", "short-lived", changed.revision, changed));
  std::this_thread::sleep_for(std::chrono::milliseconds(160));
  EXPECT_FALSE(store.get("tenant", "short-lived"));
}

TEST(TasksExtension, InvalidMutationsHaveStrongExceptionSafety) {
  auto task = make_task("tenant", "atomic", TaskStatus::working, std::chrono::minutes(1), std::chrono::milliseconds(5));
  const auto before = task.wire;
  EXPECT_THROW(
      transition_task(task, TaskStatus::input_required,
                      {{"inputRequests",
                        {{"ok", {{"method", "roots/list"}, {"params", Json::object()}}}, {"bad", {{"method", 5}, {"params", Json::object()}}}}}}),
      std::invalid_argument);
  EXPECT_EQ(task.wire, before);
  transition_task(
      task, TaskStatus::input_required,
      {{"inputRequests",
        {{"one", {{"method", "roots/list"}, {"params", Json::object()}}}, {"two", {{"method", "roots/list"}, {"params", Json::object()}}}}}});
  const auto waiting = task.wire;
  EXPECT_THROW(accept_task_inputs(task, {{"one", {{"roots", Json::array()}}}, {"two", {{"roots", "invalid"}}}}), RpcError);
  EXPECT_EQ(task.wire, waiting);
}

TEST(TasksExtension, MemoryStoreCompareExchangeIsAtomicUnderContention) {
  MemoryTaskStore store;
  auto initial = store.create(make_task("tenant", "contended", TaskStatus::working, std::chrono::minutes(1), std::chrono::milliseconds(5)));
  std::atomic<int> successes{0};
  std::vector<std::thread> workers;
  for (int i = 0; i < 12; ++i)
    workers.emplace_back([&] {
      auto replacement = initial;
      replacement.wire["statusMessage"] = "racing update";
      if (store.compare_exchange("tenant", "contended", initial.revision, std::move(replacement)))
        ++successes;
    });
  for (auto& worker : workers)
    worker.join();
  EXPECT_EQ(successes.load(), 1);
  EXPECT_EQ(store.get("tenant", "contended")->revision, initial.revision + 1);
}

TEST(TasksExtension, MemoryStoreRejectsIdentityChangesAndExpiredCompareExchange) {
  MemoryTaskStore store;
  auto task = store.create(make_task("tenant", "immutable", TaskStatus::working, std::chrono::milliseconds(100), std::chrono::milliseconds(5)));
  auto renamed = task;
  renamed.wire["taskId"] = "other";
  EXPECT_THROW(store.compare_exchange("tenant", "immutable", task.revision, renamed), std::invalid_argument);
  EXPECT_EQ(store.get("tenant", "immutable")->revision, task.revision);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  EXPECT_FALSE(store.compare_exchange("tenant", "immutable", task.revision, task));
  EXPECT_FALSE(store.get("tenant", "immutable"));
}

TEST(TasksExtension, RegisteredMethodsRequireNegotiatedCapabilityAndTrustedOwner) {
  auto store = std::make_shared<MemoryTaskStore>();
  auto task = store->create(make_task("issuer\nsubject", "job-1", TaskStatus::working, std::chrono::minutes(1), std::chrono::milliseconds(1)));
  (void)task;
  Server server;
  install_tasks(server, store);
  auto request = make_request(10, "tasks/get", {{"taskId", "job-1"}}, {{"extensions", {{std::string(tasks_id), Json::object()}}}});
  DispatchOptions options;
  options.attributes = {{"iss", "issuer"}, {"sub", "subject"}};
  auto response = server.dispatch(request, options);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["status"], "working");
  auto unsupported = server.dispatch(make_request(11, "tasks/get", {{"taskId", "job-1"}}), options);
  EXPECT_EQ(unsupported["error"]["code"], static_cast<int>(ErrorCode::missing_capability));
  auto anonymous = server.dispatch(make_request(12, "tasks/get", {{"taskId", "job-1"}}, {{"extensions", {{std::string(tasks_id), Json::object()}}}}));
  EXPECT_EQ(anonymous["error"]["code"], static_cast<int>(ErrorCode::missing_capability));
}

TEST(SkillsExtension, BuildsStableManifestAndRejectsTraversal) {
  TempDirectory temp;
  const auto root = temp.path / "skills";
  std::filesystem::create_directories(root / "demo" / "references");
  {
    std::ofstream skill(root / "demo" / "SKILL.md", std::ios::binary);
    skill << "---\nname: demo\ndescription: A test skill\n---\n# Demo\n";
    std::ofstream ref(root / "demo" / "references" / "note.md", std::ios::binary);
    ref << "note\n";
  }
  SkillDirectory skills(root);
  const auto listing = skills.list();
  ASSERT_EQ(listing["skills"].size(), 1);
  EXPECT_EQ(listing["skills"][0]["frontmatter"]["name"], "demo");
  EXPECT_EQ(listing["skills"][0]["resources"].size(), 2);
  EXPECT_EQ(skills.read("skill://demo/SKILL.md")["contents"][0]["mimeType"], "text/markdown");
  EXPECT_THROW(skills.read("skill://demo/../outside"), RpcError);
  EXPECT_THROW(skills.get("skill://demo/not-skill.md"), RpcError);
  std::filesystem::remove_all(root);
}

TEST(SkillsExtension, StableListPaginatesWithCacheMetadataAndRejectsStaleCursors) {
  TempDirectory temp;
  const auto root = temp.path / "skills";
  for (const auto* name : {"alpha", "bravo", "charlie"}) {
    std::filesystem::create_directories(root / name);
    std::ofstream skill(root / name / "SKILL.md", std::ios::binary);
    skill << "---\nname: " << name << "\ndescription: test\n---\n";
  }
  SkillListOptions options;
  options.page_size = 2;
  options.ttl_ms = 45000;
  options.cache_scope = "public";
  SkillDirectory skills(root, "skill://", {}, options);
  auto first = skills.list();
  ASSERT_EQ(first["skills"].size(), 2);
  ASSERT_TRUE(first.contains("nextCursor"));
  EXPECT_EQ(first["ttlMs"], 45000);
  EXPECT_EQ(first["cacheScope"], "public");
  auto second = skills.list(text(first["nextCursor"]));
  ASSERT_EQ(second["skills"].size(), 1);
  EXPECT_FALSE(second.contains("nextCursor"));
  EXPECT_EQ(skills.resource_descriptors().size(), 3);
  EXPECT_THROW(skills.list("garbage"), RpcError);

  std::ofstream changed(root / "charlie" / "SKILL.md", std::ios::binary | std::ios::trunc);
  changed << "---\nname: charlie\ndescription: changed\n---\n";
  changed.close();
  EXPECT_THROW(skills.list(text(first["nextCursor"])), RpcError);
}

TEST(AppsExtension, EmitsStableMetadataAndValidatesEnumeratedBridgeMessages) {
  EXPECT_EQ(ui_capability()[std::string(apps_id)]["mimeTypes"][0], std::string(apps_mime_type));
  auto metadata = ui_tool_metadata("ui://demo/view.html");
  EXPECT_EQ(metadata["ui"]["resourceUri"], "ui://demo/view.html");
  auto request = ui_message(UiMethod::open_link, {{"url", "https://example.com"}});
  request["id"] = 7;
  EXPECT_NO_THROW(validate_ui_message(UiMethod::open_link, request, false));
  request["params"].erase("url");
  EXPECT_THROW(validate_ui_message(UiMethod::open_link, request, false), RpcError);
  EXPECT_THROW(ui_resource("ui://demo/view.html", "View", {{"permissions", {{"camera", true}}}}), std::invalid_argument);
  auto notification = ui_message(UiMethod::size_changed, {{"width", 100}});
  EXPECT_NO_THROW(validate_ui_message(UiMethod::size_changed, notification, true));
  auto draft =
      draft_ui_message(DraftUiMethod::download_file, {{"contents", Json::array({{{"uri", "file:///tmp/out.txt"}, {"name", "out.txt"}}})}}, 8);
  EXPECT_NO_THROW(validate_draft_ui_message(DraftUiMethod::download_file, draft, false));
  EXPECT_THROW(validate_ui_message(UiMethod::open_link, draft, false), RpcError);
}
