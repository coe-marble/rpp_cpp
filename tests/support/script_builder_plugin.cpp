#include "fake_plugins.hpp"

extern "C" rpp::Plugin * create_plugin()
{
  return new rpp::testing::ScriptBuilderPlugin();
}
