#include "TestSupport.h"

#include <cmath>

#include "editor/EditorCamera.h"

using namespace engine;
using tests::TestRun;

namespace {

    constexpr int   kViewportW = 1728;
    constexpr int   kViewportH = 977;
    constexpr float kCentreX   = kViewportW * 0.5f;
    constexpr float kCentreY   = kViewportH * 0.5f;

    // Zoom eases exponentially, so run enough frames for it to converge.
    void Settle(EditorCamera& camera, const float mouseX, const float mouseY)
    {
        for (int i = 0; i < 400; ++i) {
            camera.Update(1.0f / 60.0f, mouseX, mouseY, kViewportW, kViewportH);
        }
    }

} // namespace

int main()
{
    TestRun t("camera");

    const EditorConfig config;

    {   // Zoom is anchored on the screen centre, not the cursor.
        EditorCamera camera;
        camera.Configure(config, 2048, 2048);
        Settle(camera, kCentreX, kCentreY);

        float beforeX = 0.0f, beforeY = 0.0f;
        camera.ScreenToWorld(kCentreX, kCentreY, beforeX, beforeY);
        const float zoomBefore = camera.Zoom();

        camera.OnMouseWheel(3.0f);
        Settle(camera, 20.0f, 15.0f);  // cursor parked in a corner

        float afterX = 0.0f, afterY = 0.0f;
        camera.ScreenToWorld(kCentreX, kCentreY, afterX, afterY);

        CHECK(t, "zoom changed", camera.Zoom() > zoomBefore * 1.5f);
        CHECK(t, "the world point at screen centre is unmoved",
              std::fabs(afterX - beforeX) < 0.01f && std::fabs(afterY - beforeY) < 0.01f);
    }

    {   // The same wheel input from any cursor position must agree exactly.
        const auto zoomFrom = [&](const float mouseX, const float mouseY) {
            EditorCamera camera;
            camera.Configure(config, 2048, 2048);
            Settle(camera, mouseX, mouseY);
            camera.OnMouseWheel(-2.0f);
            Settle(camera, mouseX, mouseY);
            return std::tuple{camera.X(), camera.Y(), camera.Zoom()};
        };
        const auto corner = zoomFrom(0.0f, 0.0f);
        const auto centre = zoomFrom(kCentreX, kCentreY);
        const auto far    = zoomFrom(static_cast<float>(kViewportW),
                                     static_cast<float>(kViewportH));
        CHECK(t, "camera position is independent of the cursor",
              corner == centre && corner == far);
    }

    {   // Zooming out and back must not accumulate drift.
        EditorCamera camera;
        camera.Configure(config, 2048, 2048);
        Settle(camera, 100.0f, 700.0f);
        const float startX = camera.X();
        const float startY = camera.Y();

        camera.OnMouseWheel(4.0f);
        Settle(camera, 100.0f, 700.0f);
        camera.OnMouseWheel(-4.0f);
        Settle(camera, 100.0f, 700.0f);

        CHECK(t, "no positional drift across zoom in and out",
              camera.X() == startX && camera.Y() == startY);
    }

    {
        EditorCamera camera;
        camera.Configure(config, 2048, 2048);
        Settle(camera, kCentreX, kCentreY);
        const float before = camera.X();
        camera.SetPanInput(1.0f, 0.0f);
        camera.Update(0.1f, kCentreX, kCentreY, kViewportW, kViewportH);
        CHECK(t, "panning still moves the camera", camera.X() > before);
    }

    return t.Result();
}
