#include "null_video_renderer.h"

namespace {
int setup(int, int, int, int, void*, int) { return 0; }
void noop() {}
int discard(PDECODE_UNIT) { return DR_OK; }
}

DECODER_RENDERER_CALLBACKS makeNullVideoRenderer() {
    DECODER_RENDERER_CALLBACKS callbacks;
    LiInitializeVideoCallbacks(&callbacks);
    callbacks.setup = setup;
    callbacks.start = noop;
    callbacks.stop = noop;
    callbacks.cleanup = noop;
    callbacks.submitDecodeUnit = discard;
    // Avoid an extra decoder queue: received encoded frames are immediately
    // acknowledged and discarded on moonlight-common-c's receive thread.
    callbacks.capabilities = CAPABILITY_DIRECT_SUBMIT;
    return callbacks;
}
