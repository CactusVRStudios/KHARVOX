#pragma once
#include <vector>
namespace kharvox::native {
// The engine caches source handles, but Vulkan has eye-private clones bound.
// A new root must re-resolve inherited source bindings for its eye even when
// the engine sees no handle change. State commands contain original handles.
// Reset the command-buffer set at the frame boundary, not at every pass.
template<class CommandBuffer, class Seen, class States>
bool restoreEyeBindingsOnce(CommandBuffer cb, Seen& seen, const States& states) {
    if (!seen.insert(cb).second) return false;
    // Match deferred replay ownership: a callback may re-enter bookkeeping.
    std::vector<typename States::mapped_type> commands;
    commands.reserve(states.size());
    for (const auto& [key, command] : states) commands.push_back(command);
    for (const auto& command : commands) command();
    return true;
}
}
