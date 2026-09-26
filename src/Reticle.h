#pragma once

// Draws the reticle cross into the currently bound draw framebuffer, centred
// on an NDC position (-1..1, +y up) and sized off the framebuffer's height.
//
// Scissored clears rather than geometry: no shaders, buffers or VAOs, so
// nothing can leak into Hydra's next pass. Saves and restores the scissor box
// and clear colour, and leaves GL_SCISSOR_TEST disabled -- the caller owns
// restoring its original enable state.
//
// Shared by the XR presenter and the offscreen --reticle path, so a test
// image exercises the same code the headset does.
void DrawReticle(int framebufferWidth, int framebufferHeight, float ndcX, float ndcY);
