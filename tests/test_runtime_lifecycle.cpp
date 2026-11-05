#include <gtest/gtest.h>
#include <stdexcept>

#include "rpp_cpp/plugin_runtime.hpp"

TEST(RuntimeLifecycle, RpcAddressesRootsAndDoesNotRepeatCompletedCallbacks)
{
  auto io = kj::setupAsyncIo();
  auto server = kj::heap<rpp::PluginRuntimeServer>();
  int configured = 0, cleaned = 0, other_configured = 0;
  server->register_component(
    "first", [&]() {++configured; return rpp::CallbackResult{};},
    [&]() {++cleaned; return rpp::CallbackResult{};});
  server->register_component(
    "second", [&]() {++other_configured; return rpp::CallbackResult{};},
    []() {return rpp::CallbackResult{};});
  rpp::runtime::PluginRuntime::Client client(kj::mv(server));
  EXPECT_EQ(configured, 0);
  auto version = client.getProtocolVersionRequest().send().wait(io.waitScope);
  EXPECT_EQ(version.getVersion(), rpp::runtime::PROTOCOL_VERSION);
  auto missing = client.configureComponentRequest();
  missing.setName("missing");
  EXPECT_FALSE(missing.send().wait(io.waitScope).getResult().getSuccess());
  for (int i = 0; i < 2; ++i) {
    auto request = client.configureComponentRequest();
    request.setName("first");
    EXPECT_TRUE(request.send().wait(io.waitScope).getResult().getSuccess());
  }
  EXPECT_EQ(configured, 1);
  EXPECT_EQ(other_configured, 0);
  for (int i = 0; i < 2; ++i) {
    auto request = client.cleanupComponentRequest();
    request.setName("first");
    EXPECT_TRUE(request.send().wait(io.waitScope).getResult().getSuccess());
  }
  EXPECT_EQ(cleaned, 1);
}

TEST(RuntimeLifecycle, CallbackExceptionAndCleanupFailureAllowExplicitRecovery)
{
  rpp::PluginRuntimeServer server;
  int attempts = 0;
  bool fail_cleanup = true;
  server.register_component(
    "root", [&]() -> rpp::CallbackResult {
      if (++attempts == 1) {throw std::runtime_error("configuration rejected");}
      return {};
    }, [&]() {
      return fail_cleanup ? rpp::CallbackResult::failure(
        "still owns resources") : rpp::CallbackResult{};
    });
  EXPECT_EQ(server.configure_component("root").message, "configuration rejected");
  EXPECT_FALSE(server.configure_component("root").success);
  EXPECT_EQ(attempts, 1);
  EXPECT_FALSE(server.cleanup_component("root").success);
  EXPECT_FALSE(server.configure_component("root").success);
  fail_cleanup = false;
  EXPECT_TRUE(server.cleanup_component("root").success);
  EXPECT_TRUE(server.configure_component("root").success);
  EXPECT_EQ(attempts, 2);
}
