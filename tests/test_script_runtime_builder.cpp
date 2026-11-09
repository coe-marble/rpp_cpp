#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "rpp_cpp/component_runtime_builder.hpp"
#include "rpp_cpp/rpp_paths.hpp"
#include "support/fake_plugins.hpp"

namespace
{
namespace fs = std::filesystem;
using rpp::testing::ScriptBuilderPlugin;
using json = nlohmann::json;

static_assert(!std::is_same_v<rpp::ComponentRuntime<>, rpp::ScriptRuntime<>>);
static_assert(!std::is_same_v<rpp::ComponentRuntime<true>, rpp::ScriptRuntime<true>>);

template<class Threading>
struct RuntimeBuilderReturnTypes
{
  using Builder = rpp::BasicComponentRuntimeBuilder<Threading>;
  using Component = rpp::ComponentRuntime<Threading::concurrent>;
  using Script = rpp::ScriptRuntime<Threading::concurrent>;

  static_assert(std::is_same_v<
      decltype(std::declval<Builder &>().build_component_from_path(std::declval<const std::string &>())),
      Component>);
  static_assert(std::is_same_v<
      decltype(std::declval<Builder &>().build_script_from_path(std::declval<const std::string &>())),
      Script>);
};

template struct RuntimeBuilderReturnTypes<rpp::SingleThreaded>;
template struct RuntimeBuilderReturnTypes<rpp::Concurrent>;

template<class Threading>
class ScriptRuntimeBuilderTest : public ::testing::Test
{
public:
  using Builder = rpp::BasicComponentRuntimeBuilder<Threading>;

  fs::path workspace;
  fs::path description_path;
  fs::path script_path;
  std::unique_ptr<rpp::RppDataManager> data_manager;
  std::string previous_home;

  static void write_file(const fs::path & path, const std::string & contents)
  {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    if (!output) {throw std::runtime_error("Cannot create fixture file: " + path.string());}
    output << contents;
    output.close();
    if (!output) {throw std::runtime_error("Cannot write fixture file: " + path.string());}
  }

  static void write_component(const fs::path & parts, const std::string & id,
    const std::string & name, int value)
  {
    const auto folder = parts / "tests__script_plugin" / id;
    const json description = {
      {"Id", id}, {"Name", name}, {"PluginType", "tests::ScriptPlugin"},
      {"PluginName", "tests::script_plugin"}, {"Library", "tests"},
      {"SubcomponentSpec", json::object()}, {"Subcomponents", json::object()}
    };
    write_file(folder / "description.json", description.dump(2));
    write_file(folder / "params" / "parameters.py",
      "class ComponentParameters:\n    value = " + std::to_string(value) + "\n");
  }

  void SetUp() override
  {
    previous_home = rpp::RPP_HOME;
    std::random_device random;
    for (int attempt = 0; attempt < 10 && workspace.empty(); ++attempt) {
      const auto candidate = fs::temp_directory_path() /
        ("rpp_script_builder_" + std::to_string(random()));
      if (fs::create_directory(candidate)) {workspace = candidate;}
    }
    if (workspace.empty()) {throw std::runtime_error("Cannot create a unique test workspace");}

    const auto home = workspace / "home";
    const auto registry = home / "registry";
    const fs::path plugin_library(TEST_PLUGIN_LIBRARY);
    const auto relative_library = fs::path("native") / plugin_library.filename();
    fs::create_directories((registry / relative_library).parent_path());
    fs::copy_file(plugin_library, registry / relative_library);
    const json plugin = {
      {"PluginName", "tests::script_plugin"}, {"SourceLanguage", "cpp"},
      {"Library", "tests"}, {"PluginType", "tests::ScriptPlugin"},
      {"ClassName", "ScriptBuilderPlugin"},
      {"PluginSharedLibraryPath", relative_library.generic_string()},
      {"PluginMetadata", {{"Parameters", {{"value", {{"type", "int"}, {"default_value", -1}}}}}}}
    };
    write_file(registry / "libraries" / "tests" / "rpp_plugins" / "script_plugin.json",
      plugin.dump(2));

    script_path = workspace / "example.cpp";
    description_path = workspace / ".rppws" / "script_descriptions" / "example.json";
    write_file(script_path, "// Script source is not executed during runtime construction.\n");
    const json primary = {{"Id", "primary-id"}, {"PluginName", "tests::script_plugin"}};
    const json backup = {{"Id", "backup-id"}, {"PluginName", "tests::script_plugin"}};
    const json description = {
      {"ScriptName", "tests::example"}, {"ScriptPath", script_path.string()},
      {"Language", "cpp"},
      {"Spec", {{"controller", "Option[tests::ScriptPlugin]"},
        {"unused", "List[tests::ScriptPlugin]"}}},
      {"Configurations", {
        {"Both", {{"Components", {{"controller", json::array({primary, backup})}}}}},
        {"BackupOnly", {{"Components", {{"controller", json::array({backup})}}}}}
      }},
      {"ActiveConfiguration", "Both"}
    };
    write_file(description_path, description.dump(2));
    const auto parts = workspace / ".rppws" / "parts";
    write_component(parts, "primary-id", "primary", 10);
    write_component(parts, "backup-id", "backup", 20);
    rpp::RPP_HOME = home.string();
    data_manager = std::make_unique<rpp::RppDataManager>(home.string(), workspace.string());
  }

  void TearDown() override
  {
    data_manager.reset();
    rpp::RPP_HOME = previous_home;
    if (!workspace.empty()) {
      std::error_code error;
      fs::remove_all(workspace, error);
      EXPECT_FALSE(error) << "Cannot remove test workspace: " << error.message();
    }
  }
};

struct PolicyNames
{
  template<class Policy>
  static std::string GetName(int) {return Policy::concurrent ? "Concurrent" : "SingleThreaded";}
};
using Policies = ::testing::Types<rpp::SingleThreaded, rpp::Concurrent>;
TYPED_TEST_SUITE(ScriptRuntimeBuilderTest, Policies, PolicyNames);

TYPED_TEST(ScriptRuntimeBuilderTest, ScriptPathLoadsActiveConfigurationAndDrivesRealPluginLifecycle)
{
  using Builder = typename TestFixture::Builder;
  auto runtime = [&] {
    Builder builder(*this->data_manager, rpp::ClockOptions{}, true);
    return builder.build_script_from_path(this->script_path.string());
  }();  // The runtime and its slots must outlive the builder.
  const rpp::ComponentContext & context = runtime.get_context();
  EXPECT_EQ(context.list_subcomponents(), (std::vector<std::string>{"controller", "unused"}));
  EXPECT_TRUE(runtime.get_subcomponent_statuses("unused").empty());
  const auto initial = runtime.get_subcomponent_statuses("controller");
  ASSERT_EQ(initial.size(), 2u);
  EXPECT_EQ(initial[0].name, "primary");
  EXPECT_EQ(initial[1].name, "backup");
  EXPECT_NE(initial[0].instance_id, initial[1].instance_id);
  for (const auto & entry : initial) {EXPECT_EQ(entry.state, rpp::LifecycleState::UNCONFIGURED);}
  auto primary = context.get_subcomponent<ScriptBuilderPlugin>("controller", "primary");
  auto backup = context.get_subcomponent<ScriptBuilderPlugin>("controller", "backup");
  EXPECT_EQ(primary->configure_calls, 0);
  EXPECT_EQ(backup->configure_calls, 0);
  EXPECT_EQ(primary->activate_calls, 0);
  EXPECT_EQ(backup->activate_calls, 0);

  ASSERT_TRUE(runtime.configure().success);
  EXPECT_EQ(primary->configure_calls, 1);
  EXPECT_EQ(backup->configure_calls, 1);
  EXPECT_EQ(primary->value, 10);
  EXPECT_EQ(backup->value, 20);
  ASSERT_TRUE(context.activate_subcomponents().success);
  EXPECT_EQ(primary->activate_calls, 1);
  EXPECT_EQ(backup->activate_calls, 0);
  EXPECT_EQ(context.with_active_component<ScriptBuilderPlugin>(
      "controller", "primary", [](auto & plugin) {return plugin.value;}), 10);

  ASSERT_TRUE(runtime.switch_component("controller", "primary", "backup").success);
  EXPECT_EQ(primary->deactivate_calls, 1);
  EXPECT_EQ(backup->activate_calls, 1);
  EXPECT_EQ(context.get_subcomponent<ScriptBuilderPlugin>("controller"), backup);
  const auto backup_view = context.get_subcomponent_contexts("controller").back();
  EXPECT_TRUE(backup_view->is_active());
  ASSERT_TRUE(runtime.close().success);
  EXPECT_EQ(primary->deactivate_calls, 1);
  EXPECT_EQ(backup->deactivate_calls, 1);
  EXPECT_EQ(primary->cleanup_calls, 1);
  EXPECT_EQ(backup->cleanup_calls, 1);
  EXPECT_THROW(backup_view->get_instance<ScriptBuilderPlugin>(), rpp::LifecycleError);
}

TYPED_TEST(ScriptRuntimeBuilderTest, DescriptionPathUsesExplicitConfigurationAndPartsOverride)
{
  using Builder = typename TestFixture::Builder;
  const auto alternate_parts = this->workspace / "alternate_parts";
  this->write_component(alternate_parts, "backup-id", "backup", 42);
  auto runtime = [&] {
    Builder builder(*this->data_manager);
    return builder.build_script_from_description_path(
      this->description_path.string(), "BackupOnly", alternate_parts.string());
  }();
  const auto entries = runtime.get_subcomponent_statuses("controller");
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries.front().name, "backup");
  const rpp::ComponentContext & context = runtime.get_context();
  auto backup = context.get_subcomponent<ScriptBuilderPlugin>("controller");
  ASSERT_TRUE(runtime.configure().success);
  EXPECT_EQ(backup->value, 42);
  ASSERT_TRUE(context.activate_subcomponents().success);
  ASSERT_TRUE(runtime.close().success);
  EXPECT_EQ(backup->activate_calls, 1);
  EXPECT_EQ(backup->deactivate_calls, 1);
  EXPECT_EQ(backup->cleanup_calls, 1);
}

TYPED_TEST(ScriptRuntimeBuilderTest, LibraryLookupFindsScriptAndHonorsConfigurationOverride)
{
  using Builder = typename TestFixture::Builder;
  auto runtime = [&] {
    Builder builder(*this->data_manager);
    return builder.build_script_from_library("tests", "tests::example", "BackupOnly");
  }();
  const auto entries = runtime.get_subcomponent_statuses("controller");
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries.front().name, "backup");
  const rpp::ComponentContext & context = runtime.get_context();
  auto backup = context.get_subcomponent<ScriptBuilderPlugin>("controller");
  ASSERT_TRUE(runtime.configure().success);
  EXPECT_EQ(backup->value, 20);
  ASSERT_TRUE(context.activate_subcomponents().success);
  ASSERT_TRUE(runtime.close().success);
  EXPECT_EQ(backup->configure_calls, 1);
  EXPECT_EQ(backup->activate_calls, 1);
  EXPECT_EQ(backup->deactivate_calls, 1);
  EXPECT_EQ(backup->cleanup_calls, 1);
}
}  // namespace
