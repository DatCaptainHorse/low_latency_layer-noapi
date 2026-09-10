#include "instance_context.hh"

#include "layer_context.hh"

#include <utility>

namespace low_latency {

// The per-application list of "known decoupled simulation" executables used to
// live here, gating an extra adaptive delay for one game. Both are gone: the
// delay was a feedback loop that could not observe its own cost and wound up
// until it halved the frame rate, and a hardcoded executable list is not a
// mechanism the layer can rely on now that it is always on.

InstanceContext::InstanceContext(const LayerContext& parent_context,
                                 const VkInstance& instance,
                                 const std::uint32_t& api_version,
                                 VkuInstanceDispatchTable&& vtable)
    : layer(parent_context), instance(instance), api_version(api_version),
      vtable(std::move(vtable)) {}

InstanceContext::~InstanceContext() {}

} // namespace low_latency
