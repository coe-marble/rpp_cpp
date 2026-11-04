#include "rpp_cpp/context.hpp"
#include "rpp_cpp/external_component_process.hpp"

namespace rpp
{
CallbackResult ComponentContext::close_runtime()
{
  if (!external_component_process_) {return {};}
  if (external_component_process_.use_count() == 1) {
    auto result = external_component_process_->close();
    if (!result.success) {return result;}
  }
  external_component_process_.reset();
  return {};
}
}  // namespace rpp
