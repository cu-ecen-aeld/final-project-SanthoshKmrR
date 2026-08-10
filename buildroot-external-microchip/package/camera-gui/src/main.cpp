/*
 * main.cpp  --  camera-gui  (SPRINT 1: "Hello" text + a simple button UI)
 *
 *  Author: Santhosh Kumar
 *
 * ===========================================================================
 * SPRINT 1 SCOPE
 * ===========================================================================
 * The very first milestone of the camera-gui project. The goal here is ONLY to
 * stand up a working Buildroot external package that cross-compiles an EGT
 * (Ensemble Graphics Toolkit) application for the SAMA7D65 Curiosity board and
 * draws something on the LVDS panel:
 *
 *   * a "Hello" text label, and
 *   * one touch Button that, when pressed, updates a status label (a press
 *     counter) -- proving the EGT event loop and touch input work end-to-end.
 *
 * There is NO video, NO network and NO GStreamer yet. Those arrive in later
 * sprints:
 *   Sprint 2 -> add a UDP :5001 "PIR / motion sensor" listener that prints
 *               "Sensor Detected" in the UI.
 *   Sprint 3 -> add the GStreamer RTP video overlay, JPEG capture, and syslog.
 *
 * -------------------------- BUILD / TOOLCHAIN -----------------------------
 * Cross-compiled for arm-buildroot-linux-gnueabihf. Compile/link flags come
 * from the buildroot pkg-config so the TARGET sysroot .pc files are used:
 *   pkg-config libegt --cflags   (compile)
 *   pkg-config libegt --libs     (link)
 * ===========================================================================
 */

#include <egt/ui>       // umbrella header: Application, Window, Button, Label

#include <cstdio>       // std::fprintf / vsnprintf for the logger
#include <cstdarg>      // va_list / va_start for the variadic logger
#include <memory>       // std::shared_ptr / std::make_shared
#include <string>

namespace {

/* --------------------------------------------------------------------------
 * Compile-time configuration.
 *   kWidth/kHeight : display panel size (WVGA LVDS panel = 800x480).
 * -------------------------------------------------------------------------- */
constexpr int kWidth  = 800;   // full LVDS panel width
constexpr int kHeight = 480;   // full LVDS panel height

/* ==========================================================================
 * Logging (Sprint 1: stderr only)
 * --------------------------------------------------------------------------
 * A tiny variadic logger so every stage prints a tagged line to stderr,
 * visible on the serial console. (Sprint 3 additionally mirrors these lines
 * into syslog; for now stderr is enough.)
 * ========================================================================== */
void log_line(const char* tag, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);   // safe, bounded formatting
    va_end(ap);
    std::fprintf(stderr, "[camera-gui][%s] %s\n", tag, buf);
    std::fflush(stderr);                    // flush so lines appear immediately
}

#define LOGI(...) log_line("INFO",  __VA_ARGS__)
#define LOGW(...) log_line("WARN",  __VA_ARGS__)
#define LOGE(...) log_line("ERROR", __VA_ARGS__)

} // namespace

int main(int argc, char** argv)
{
    LOGI("==== camera-gui starting (Sprint 1, build %s %s) ====", __DATE__, __TIME__);

    // Create the EGT application object. This brings up the screen/KMS backend
    // and the event loop. Must exist before any window is created.
    egt::Application app(argc, argv);
    LOGI("egt::Application created (screen backend up)");

    // Root / primary plane, painted black.
    egt::TopWindow win;
    win.color(egt::Palette::ColorId::bg, egt::Palette::black);

    // ---- "Hello" text, centred near the top of the panel ------------------
    egt::Label hello("Hello, camera-gui!",
        egt::Rect(egt::Point(0, 120), egt::Size(kWidth, 60)));
    hello.color(egt::Palette::ColorId::label_text, egt::Palette::white);
    win.add(hello);
    LOGI("hello label added");

    // ---- Status label: reflects button presses ----------------------------
    egt::Label status("Press the button",
        egt::Rect(egt::Point(0, 200), egt::Size(kWidth, 40)));
    status.color(egt::Palette::ColorId::label_text, egt::Color(0xb0b0b0ff));
    win.add(status);

    // ---- Capture-style button, horizontally centred ------------------------
    // Named "Capture" already, so the control that later triggers a JPEG grab
    // is present from Sprint 1; here it just increments a press counter.
    constexpr int kBtnW = 200, kBtnH = 70;
    egt::Button button("Capture",
        egt::Rect(egt::Point((kWidth - kBtnW) / 2, 280),
                  egt::Size(kBtnW, kBtnH)));
    win.add(button);

    // Press counter lives in a shared_ptr so the lambda can own a reference
    // that outlives this scope (same pattern the later sprints use for the
    // pump-timer counters).
    auto count = std::make_shared<unsigned long>(0);
    button.on_click([&status, count](egt::Event&) {
        ++*count;
        status.text("Button pressed " + std::to_string(*count) + " time(s)");
        LOGI("button pressed (count=%lu)", *count);
    });
    LOGI("capture button wired");

    win.show();                          // show the primary plane
    LOGI("entering app.run()");
    int rc = app.run();                  // enter EGT's event loop (blocks)
    LOGI("==== camera-gui exit rc=%d ====", rc);
    return rc;
}
