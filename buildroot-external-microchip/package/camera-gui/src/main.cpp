/*
 * main.cpp  --  camera-gui  (SPRINT 2: + PIR / motion sensor over UDP :5001)
 *
 *  Author: Santhosh Kumar
 *
 * ===========================================================================
 * SPRINT 2 SCOPE
 * ===========================================================================
 * Builds directly on Sprint 1 (the EGT "Hello" label + touch button). This
 * sprint adds a PIR / motion-sensor notifier:
 *
 *   * A background thread listens on UDP port 5001 (INADDR_ANY). Any datagram
 *     received there is treated as a motion/PIR "detection".
 *   * The GUI shows "Sensor Detected" (in yellow) below the button for a few
 *     seconds after each detection, then clears it.
 *
 * Still NO video and NO GStreamer -- that is Sprint 3, which also mirrors the
 * logs into syslog. Logging here remains stderr-only.
 *
 * -------------------------- THREADING NOTE --------------------------------
 * EGT (like most GUI toolkits) is NOT thread-safe. So the listener thread does
 * the ONE thing that is safe off the UI thread: block in recvfrom() and flip an
 * atomic flag. A UI-thread PeriodicTimer ("pump", ~30 Hz) polls that flag and
 * does all widget updates. Result: no cross-thread access to EGT objects, and
 * no locking needed.
 *
 * -------------------------- BUILD / TOOLCHAIN -----------------------------
 * Cross-compiled for arm-buildroot-linux-gnueabihf.
 *   pkg-config libegt --cflags   (compile, plus -pthread)
 *   pkg-config libegt --libs     (link,    plus -pthread)
 * ===========================================================================
 */

#include <egt/ui>       // umbrella header: Application, Window, Button, Label, timers

#include <cstdio>       // std::fprintf / vsnprintf for the logger
#include <cstdarg>      // va_list / va_start for the variadic logger
#include <cstring>      // std::strerror
#include <cerrno>       // errno
#include <memory>       // std::shared_ptr / std::make_shared
#include <string>

#include <atomic>       // std::atomic flag shared with the sensor thread
#include <thread>       // std::thread for the UDP sensor listener

#include <sys/socket.h> // socket / bind / recvfrom (UDP sensor listener)
#include <sys/time.h>   // struct timeval (SO_RCVTIMEO)
#include <netinet/in.h> // sockaddr_in, htons, INADDR_ANY
#include <arpa/inet.h>  // htonl
#include <unistd.h>     // close

namespace {

/* --------------------------------------------------------------------------
 * Compile-time configuration.
 *   kWidth/kHeight     : display panel size (WVGA LVDS panel = 800x480).
 *   kSensorPort        : UDP port the background thread listens on for the
 *                        PIR / motion "detected" notifier.
 *   kSensorRevertTicks : how long "Sensor Detected" stays on screen, measured
 *                        in ~33ms pump ticks (90 ~= 3 seconds).
 * -------------------------------------------------------------------------- */
constexpr int kWidth  = 800;
constexpr int kHeight = 480;
constexpr int kSensorPort = 5001;
constexpr unsigned long kSensorRevertTicks = 90;

/* ==========================================================================
 * Logging (Sprint 2: stderr only)
 * ========================================================================== */
void log_line(const char* tag, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[camera-gui][%s] %s\n", tag, buf);
    std::fflush(stderr);
}

#define LOGI(...) log_line("INFO",  __VA_ARGS__)
#define LOGW(...) log_line("WARN",  __VA_ARGS__)
#define LOGE(...) log_line("ERROR", __VA_ARGS__)

/* ==========================================================================
 * sensor_thread_main  --  UDP :5001 "sensor detected" listener
 * --------------------------------------------------------------------------
 * Runs on its OWN thread (started from main). It NEVER touches an EGT widget;
 * it only sets *detected. The UI-thread pump timer polls that flag and updates
 * the on-screen label, so all EGT access stays single-threaded.
 *
 *   running  : owned by main; set false at shutdown. A 1-second socket receive
 *              timeout (SO_RCVTIMEO) makes recvfrom() return periodically so
 *              the loop notices and exits without closing the socket elsewhere.
 *   detected : set true on every datagram; the pump consumes it via
 *              exchange(false).
 *
 * Any datagram (of any content) counts as a detection; the payload is only
 * logged. Bind to INADDR_ANY so it works whether the sender is local or remote.
 * ========================================================================== */
void sensor_thread_main(std::atomic<bool>* running, std::atomic<bool>* detected)
{
    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        LOGE("sensor: socket() failed: %s", std::strerror(errno));
        return;
    }

    // Allow an immediate rebind if the app is restarted while the port lingers.
    int one = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    // 1s receive timeout: recvfrom() wakes at least once per second so we can
    // observe the running flag and exit promptly at shutdown.
    struct timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(kSensorPort));
    if (::bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
        LOGE("sensor: bind(:%d) failed: %s", kSensorPort, std::strerror(errno));
        ::close(sock);
        return;
    }
    LOGI("sensor: listening for UDP text on port %d", kSensorPort);

    char buf[256];
    while (running->load()) {
        const ssize_t n = ::recvfrom(sock, buf, sizeof buf - 1, 0, nullptr, nullptr);
        if (n > 0) {
            buf[n] = '\0';                 // NUL-terminate the received text
            detected->store(true);         // hand the detection to the UI thread
            LOGI("sensor: datagram received (%zd bytes): \"%s\"", n, buf);
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            // EAGAIN/EWOULDBLOCK is just our 1s timeout expiring -> loop again.
            LOGW("sensor: recvfrom failed: %s", std::strerror(errno));
        }
    }

    ::close(sock);
    LOGI("sensor: thread exiting");
}

} // namespace

int main(int argc, char** argv)
{
    LOGI("==== camera-gui starting (Sprint 2, build %s %s) ====", __DATE__, __TIME__);

    egt::Application app(argc, argv);
    LOGI("egt::Application created (screen backend up)");

    egt::TopWindow win;
    win.color(egt::Palette::ColorId::bg, egt::Palette::black);

    // ---- "Hello" text (from Sprint 1) -------------------------------------
    egt::Label hello("Hello, camera-gui!",
        egt::Rect(egt::Point(0, 100), egt::Size(kWidth, 60)));
    hello.color(egt::Palette::ColorId::label_text, egt::Palette::white);
    win.add(hello);

    // ---- Status label: reflects button presses (from Sprint 1) ------------
    egt::Label status("Press the button",
        egt::Rect(egt::Point(0, 180), egt::Size(kWidth, 40)));
    status.color(egt::Palette::ColorId::label_text, egt::Color(0xb0b0b0ff));
    win.add(status);

    // ---- Capture button (from Sprint 1) -----------------------------------
    constexpr int kBtnW = 200, kBtnH = 70;
    egt::Button button("Capture",
        egt::Rect(egt::Point((kWidth - kBtnW) / 2, 250),
                  egt::Size(kBtnW, kBtnH)));
    win.add(button);

    auto count = std::make_shared<unsigned long>(0);
    button.on_click([&status, count](egt::Event&) {
        ++*count;
        status.text("Button pressed " + std::to_string(*count) + " time(s)");
        LOGI("button pressed (count=%lu)", *count);
    });

    // ---- NEW in Sprint 2: "Sensor Detected" label -------------------------
    // Positioned just below the button. Starts blank; the pump timer fills it
    // in when the UDP sensor thread reports a detection, then clears it a few
    // seconds later. Yellow so it stands out from the white status label above.
    egt::Label sensor("",
        egt::Rect(egt::Point(0, 340), egt::Size(kWidth, 40)));
    sensor.color(egt::Palette::ColorId::label_text, egt::Palette::yellow);
    win.add(sensor);

    /* ----------------------------------------------------------------------
     * Start the UDP sensor listener thread (port kSensorPort = 5001).
     *   sensor_running : cleared at shutdown so the thread's loop exits.
     *   sensor_detected: flipped true by the thread on each datagram; the pump
     *                    below consumes it and updates the "Sensor Detected"
     *                    label. Both live for the whole of main(), and the
     *                    thread is joined before main() returns, so capturing
     *                    their addresses in the thread is safe.
     * -------------------------------------------------------------------- */
    std::atomic<bool> sensor_running{true};
    std::atomic<bool> sensor_detected{false};
    std::thread sensor_thread(sensor_thread_main, &sensor_running, &sensor_detected);

    /* ----------------------------------------------------------------------
     * UI-thread pump (~30 Hz). It polls the atomic flag the UDP thread sets and
     * drives the "Sensor Detected" label's show-then-revert behaviour. All EGT
     * widget access happens here, on the UI thread.
     *   tick_count         : pump ticks elapsed (~33ms each).
     *   sensor_revert_tick : tick at which "Sensor Detected" clears; 0 = idle.
     * -------------------------------------------------------------------- */
    auto tick_count         = std::make_shared<unsigned long>(0);
    auto sensor_revert_tick = std::make_shared<unsigned long>(0);

    egt::PeriodicTimer pump(std::chrono::milliseconds(33));
    pump.on_timeout([&, tick_count, sensor_revert_tick]() {
        ++*tick_count;

        // Consume the atomic flag the UDP thread set (if any) and show the
        // notice; running this here keeps all EGT access single-threaded.
        if (sensor_detected.exchange(false)) {
            sensor.text("Sensor Detected");
            *sensor_revert_tick = *tick_count + kSensorRevertTicks;
            LOGI("sensor: detection surfaced in UI");
        }
        // Clear the label once its display window elapses.
        if (*sensor_revert_tick != 0 && *tick_count >= *sensor_revert_tick) {
            sensor.text("");
            *sensor_revert_tick = 0;
        }
    });
    pump.start();
    LOGI("pump timer started (33ms); entering app.run()");

    win.show();
    int rc = app.run();                  // enter EGT's event loop (blocks)
    LOGI("app.run() returned %d; shutting down", rc);

    /* ----------------------------------------------------------------------
     * Stop the UDP sensor thread: clear its run flag and join. The thread's 1s
     * socket receive timeout guarantees it observes the flag and returns within
     * ~1s, so this join does not hang.
     * -------------------------------------------------------------------- */
    sensor_running.store(false);
    if (sensor_thread.joinable())
        sensor_thread.join();
    LOGI("==== camera-gui exit rc=%d ====", rc);
    return rc;
}
