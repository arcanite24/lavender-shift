#include "hitch_sampler.h"

// The hitch sampler (src/hitch_sampler_win.cpp) samples the stalled thread's
// stack through Windows APIs; elsewhere it does nothing for now.
namespace lavender_shift::hitch {

void NoteFrame() {}

}  // namespace lavender_shift::hitch
