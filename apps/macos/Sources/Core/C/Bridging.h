// Bridging header shared by the app and the unit-test bundle.
// The app talks to the engine only through the stable C API (replaynes.h) and to the shared
// frontend core through its C API (frontend.h).
#include "replaynes.h"
#include "frontend.h"
#include "rn_audio_ring.h"
#include "rn_frame_workgroup.h"
