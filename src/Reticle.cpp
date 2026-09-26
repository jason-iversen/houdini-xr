#include "Reticle.h"

#include <pxr/imaging/garch/glApi.h>

#include <algorithm>

PXR_NAMESPACE_USING_DIRECTIVE

void DrawReticle(int framebufferWidth, int framebufferHeight, float ndcX, float ndcY)
{
    // GL window coordinates: origin bottom-left, matching NDC's +y up.
    const int cx = int((ndcX * 0.5f + 0.5f) * float(framebufferWidth));
    const int cy = int((ndcY * 0.5f + 0.5f) * float(framebufferHeight));

    // Sized off the framebuffer so it reads the same on any headset.
    const int arm       = std::max(8, framebufferHeight / 80);
    const int thickness = std::max(2, framebufferHeight / 700);
    const int outline   = 2;

    GLint   savedBox[4];
    GLfloat savedClear[4];
    glGetIntegerv(GL_SCISSOR_BOX, savedBox);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, savedClear);
    glEnable(GL_SCISSOR_TEST);

    auto cross = [&](int grow) {
        const int a = arm + grow;
        const int t = thickness + 2 * grow;
        glScissor(cx - a, cy - t / 2, 2 * a, t);
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(cx - t / 2, cy - a, t, 2 * a);
        glClear(GL_COLOR_BUFFER_BIT);
    };

    // Black outline under white, so it stays legible on any background.
    // Both are fixed points of the sRGB curve, so it doesn't matter whether
    // the target encodes.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    cross(outline);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    cross(0);

    glClearColor(savedClear[0], savedClear[1], savedClear[2], savedClear[3]);
    glScissor(savedBox[0], savedBox[1], savedBox[2], savedBox[3]);
    glDisable(GL_SCISSOR_TEST);
}
