#pragma once

namespace lavender_shift::movies {
// Registers the guest file-open observer (movie tracking, early launch
// options). Call once the kernel is up, before the title starts.
void Install();
void Uninstall();
}  // namespace lavender_shift::movies
