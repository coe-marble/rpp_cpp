#include <gtest/gtest.h>

#include <string>

#include "rpp_cpp/component_status.hpp"

TEST(CallbackResult, RequireSuccessAcceptsSuccessfulResult)
{
  const rpp::CallbackResult result;
  EXPECT_NO_THROW(result.require_success());
}

TEST(CallbackResult, RequireSuccessUsesFailureMessageWithoutLabel)
{
  try {
    rpp::CallbackResult::failure("state rejected").require_success();
    FAIL() << "Expected a failed callback to raise";
  } catch (const rpp::LifecycleError & error) {
    EXPECT_EQ(std::string(error.what()), "state rejected");
  }
  EXPECT_THROW(rpp::CallbackResult::failure("").require_success(), rpp::LifecycleError);
}

TEST(CallbackResult, RequireSuccessReportsOperationAndReason)
{
  const auto result = rpp::CallbackResult::failure("state rejected");
  try {
    result.require_success("on_activate");
    FAIL() << "Expected a failed callback to raise";
  } catch (const rpp::LifecycleError & error) {
    EXPECT_NE(std::string(error.what()).find("on_activate"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("state rejected"), std::string::npos);
  }
}
