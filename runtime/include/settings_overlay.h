#pragma once

#include <aurora/aurora.h>
#include <aurora/event.h>

namespace settings_overlay {
// Apply persistent controller settings once Aurora has discovered host devices.
void InitializeRuntimeSettings() noexcept;
// Draw the F10 settings bar before each Aurora present.
void HandleEvents(const AuroraEvent* events) noexcept;
void Draw() noexcept;
bool StartupScreenVisible() noexcept;
#if defined(__ANDROID__)
// Android has no settings top bar to dismiss the cover; the boot/strap logo
// must not be painted over the game's own frames once strap input runs.
void NotifyBootFramesVisible() noexcept;
#endif
void NotifyStrapInputAccepted() noexcept;
void AdvancePresentedFrame() noexcept;
} // namespace settings_overlay
