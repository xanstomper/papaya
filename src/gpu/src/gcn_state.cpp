#include "papaya/gpu/gcn_state.hpp"

namespace papaya::gpu {

void GcnContextState::reset() {
    *this = GcnContextState{};
}

} // namespace papaya::gpu
