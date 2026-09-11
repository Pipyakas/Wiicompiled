#include "hle_stubs.h"
#include "runtime_log.h"
#include "settings_overlay.h"

#include <atomic>
#include <cstdint>

// StrapScene::calc calls CheckInput only after the scene's loading and timing
// gates have completed. Accepting that check advances on the first eligible
// frame without depending on a Code.pul function address or guest controller
// hardware. The input DOL is RMCE01 (USA, hash-pinned in the build); CheckInput
// is reached by direct call from StrapScene::calc either way.
// Temporary: one-shot log to prove calc reaches the CheckInput call on-device.
// Remove with the GX counters once the black screen is found.
extern "C" uint32_t StrapScene__CheckInput_Skip(uint32_t scenePtr)
{
    (void)scenePtr;
    static std::atomic<int> s_checkLog{0};
    if (s_checkLog.fetch_add(1, std::memory_order_relaxed) < 4) {
        RT_LOG(RT_TAG_GX) << "StrapScene::CheckInput reached (skip -> accepted)" << std::endl;
    }
    settings_overlay::NotifyStrapInputAccepted();
    return 1;
}
PPC_NATIVE_OVERRIDE(800077C8, StrapScene__CheckInput_Skip, uint32_t, (uint32_t scenePtr), (scenePtr));
