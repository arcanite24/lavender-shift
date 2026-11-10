#pragma once

namespace lavender_shift::stall {

// Called by the title's frame hook. Starts the watchdog on first use; when no
// frame follows for lavender_stall_dump_seconds, the watchdog logs every
// guest thread's registers and guest call stack once per stall. Creating
// logs/dump-threads under the user folder asks for a dump at any time.
void NoteFrame();

}  // namespace lavender_shift::stall
