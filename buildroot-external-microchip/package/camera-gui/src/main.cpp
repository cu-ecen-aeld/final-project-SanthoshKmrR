/*
 * main.cpp  --  camera-gui
 *
 *  Created on: 3 Aug 2026
 *      Author: Santhosh Kumar 
 *
 * ===========================================================================
 * OVERVIEW
 * ===========================================================================
 * An EGT (Ensemble Graphics Toolkit) application for the SAMA7D65 Curiosity
 * board + LVDS panel that displays a live network video stream and can grab
 * still frames to the SD card.
 *
 *   * Live video : an RTP video stream received over UDP (port 5000) is
 *                  decoded/converted to BGRx and blitted onto a hardware
 *                  overlay plane (LCDC "High-End Overlay", HEO).
 *   * Capture    : a touch button opens a GStreamer "valve" for exactly one
 *                  buffer, which is JPEG-encoded and written to the SD card.
 *
 * -------------------------- ARCHITECTURE ----------------------------------
 * A single process owns the display. GStreamer is embedded through its C API
 * (rather than EGT's opaque VideoWindow/CameraWindow) so we retain full
 * control over the pipeline. One EGT PeriodicTimer runs on the UI thread and
 * does two jobs each tick:
 *     1. pull the newest decoded frame from an appsink and blit it, and
 *     2. drain the GStreamer message bus.
 * Because both happen on the UI thread, there is NO cross-thread access to
 * EGT or GStreamer objects -- which keeps the code free of locking.
 *
 * The video is drawn on a WindowHint::heo_overlay window (a dedicated hardware
 * plane). The UI (button + status label) lives on the main graphics plane with
 * a transparent background, and the LCDC composites the two planes in hardware.
 *
 * -------------------------- RUNTIME TOGGLES (env vars) --------------------
 *   CAMERA_GUI_TESTSRC=1   Use a local videotestsrc pattern (no network).
 *                          Proves the EGT/HEO display path in isolation.
 *   CAMERA_GUI_H264=1      Receive H.264 RTP (rtph264depay ! avdec_h264).
 *                          Use this with webcam_stream.sh.
 *   (neither set)          Receive raw YCbCr-4:2:2 RTP (the SAM9X75 source).
 *   CAMERA_GUI_LATENCY=ms  rtpjitterbuffer latency in milliseconds (default
 *                          200). Higher = more robust to loss but more delay.
 *   GST_DEBUG=n            Standard GStreamer debug verbosity (0..9).
 *
 * -------------------------- BUILD / TOOLCHAIN -----------------------------
 * Cross-compiled for arm-buildroot-linux-gnueabihf. Compiler/linker flags are
 * produced by pkg-config; the project MUST be built with the buildroot
 * pkg-config so the *target* sysroot .pc files are used, not the host's:
 *   compile: `pkg-config libegt gstreamer-1.0 gstreamer-app-1.0 \
 *             gstreamer-video-1.0 --cflags`
 *   link:    `pkg-config libegt gstreamer-1.0 gstreamer-app-1.0 \
 *             gstreamer-video-1.0 --libs`
 * See the accompanying PDF (Eclipse_Project_Setup.pdf) for the full IDE and
 * cross-compile configuration.
 * ===========================================================================
 */

/* --------------------------------------------------------------------------
 * Includes
 * --------------------------------------------------------------------------
 * <egt/ui>                 : umbrella header for EGT (Application, Window,
 *                            Button, Label, Surface, Image, Painter, timers).
 * <gst/gst.h>              : core GStreamer C API (init, pipeline, elements,
 *                            bus, messages, state changes).
 * <gst/app/gstappsink.h>   : the "appsink" element API -- lets the application
 *                            pull decoded buffers out of the pipeline.
 * <gst/video/video.h>      : GstVideoInfo helpers (parse caps, plane strides).
 *
 * <cstring>  : std::memcpy
 * <cstdio>   : std::fprintf / vsnprintf for the logger
 * <cstdarg>  : va_list / va_start for the variadic logger
 * <cstdlib>  : atoi (parse CAMERA_GUI_LATENCY)
 * <iostream> : std::cerr (only used as a fallback in a couple of spots)
 * <memory>   : std::shared_ptr / std::unique_ptr / std::make_shared
 * <vector>   : std::vector<uint8_t> frame buffer
 * <syslog.h> : openlog / syslog / closelog (system log integration)
 * -------------------------------------------------------------------------- */
#include <egt/ui>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cerrno>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <atomic>       // std::atomic flag shared with the sensor thread
#include <thread>       // std::thread for the UDP sensor listener

#include <sys/stat.h>   // mkdir (create the capture directory on demand)
#include <sys/socket.h> // socket / bind / recvfrom (UDP sensor listener)
#include <sys/time.h>   // struct timeval (SO_RCVTIMEO)
#include <netinet/in.h> // sockaddr_in, htons, INADDR_ANY
#include <arpa/inet.h>  // htonl
#include <unistd.h>     // close

#include <syslog.h>

namespace {

/* --------------------------------------------------------------------------
 * Compile-time configuration constants.
 *   kWidth/kHeight : display plane size (WVGA panel = 800x480).
 *   kPort          : UDP port the app listens on for the incoming RTP stream.
 *   kOutDir        : directory where captured JPEGs are stored. Resolved at
 *                    startup to "<home>/camera-gui-capture" and created on
 *                    demand (see main()). g_get_home_dir() honours $HOME and
 *                    falls back to the passwd entry, so this is
 *                    /root/camera-gui-capture when the app runs as root.
 * -------------------------------------------------------------------------- */
constexpr int  kWidth   = 800;   // full LVDS panel width
constexpr int  kHeight  = 480;   // full LVDS panel height
constexpr int  kVideoW  = 600;   // video plane width  (left-aligned region)
constexpr int  kVideoH  = 480;   // video plane height (full panel height)
constexpr int  kPort    = 5000;
// UDP port a background thread listens on for the "sensor" notifier. Any
// datagram received here makes the GUI show "Sensor Detected" below the
// Capture button for a few seconds (see the sensor thread + pump timer).
constexpr int  kSensorPort = 5001;
// How long the "Saved ..." capture status stays on screen before the label
// reverts to "Live". Measured in pump ticks (~33ms each), so 90 ~= 3 seconds.
constexpr unsigned long kStatusRevertTicks = 90;
// How long "Sensor Detected" stays on screen after a datagram arrives, in the
// same ~33ms pump ticks (90 ~= 3 seconds), before the label is cleared again.
constexpr unsigned long kSensorRevertTicks = 90;
// A capture only makes sense while a live video feed is arriving. If no frame
// has been blitted within this many pump ticks (~33ms each; 30 ~= 1 second) the
// feed is considered absent/stalled and a capture request is refused (the JPEG
// would be empty or stale). At ~30fps a frame lands roughly every tick, so 30
// ticks of silence is a comfortable "no feed" threshold.
constexpr unsigned long kFeedStaleTicks = 30;
const std::string kOutDir = std::string(g_get_home_dir()) + "/camera-gui-capture";

/* ==========================================================================
 * Logging
 * --------------------------------------------------------------------------
 * Every stage of startup and streaming is logged to BOTH stderr and syslog,
 * so progress/errors are visible whether the app is launched from a console
 * or started by an init system / service.
 *   * stderr : visible when you run the app from a shell / serial console.
 *   * syslog : visible via `logread -f` (BusyBox) or journalctl / /var/log.
 * On the target, filter with:   logread -f | grep camera-gui
 * ========================================================================== */
void log_line(int level, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);              // begin variadic argument processing
    vsnprintf(buf, sizeof(buf), fmt, ap);   // safe, bounded formatting into buf
    va_end(ap);                     // end variadic argument processing

    // Map the syslog priority to a short human-readable tag for stderr.
    const char* tag = (level <= LOG_ERR)     ? "ERROR" :
                      (level == LOG_WARNING) ? "WARN"  : "INFO";
    std::fprintf(stderr, "[camera-gui][%s] %s\n", tag, buf);
    std::fflush(stderr);            // flush so lines appear immediately (no buffering)
    syslog(level, "[%s] %s", tag, buf);     // also record in the system log
}

// Convenience macros: LOGI (info), LOGW (warning), LOGE (error).
#define LOGI(...) log_line(LOG_INFO,    __VA_ARGS__)
#define LOGW(...) log_line(LOG_WARNING, __VA_ARGS__)
#define LOGE(...) log_line(LOG_ERR,     __VA_ARGS__)

/* ==========================================================================
 * VideoPlane
 * --------------------------------------------------------------------------
 * An EGT Window bound to a hardware OVERLAY plane. It owns a CPU-side pixel
 * buffer wrapped in an egt::Surface; GStreamer decoded frames are memcpy'd
 * into that buffer and the plane is repainted. Because the window is created
 * with WindowHint::heo_overlay, the LCDC scans it out on its own plane and
 * composites it under the transparent UI plane -- no per-pixel blending in
 * software.
 * ========================================================================== */
class VideoPlane : public egt::Window
{
public:
    VideoPlane()
        // Base egt::Window ctor:
        //   Size(kWidth,kHeight)        -> plane dimensions
        //   PixelFormat::xrgb8888       -> 32bpp, 8 unused + 24 RGB (matches
        //                                  the BGRx frames we feed it)
        //   WindowHint::heo_overlay     -> request the LCDC High-End Overlay
        //                                  hardware plane (falls back to a
        //                                  generic overlay if HEO is taken)
        : egt::Window(egt::Size(kVideoW, kVideoH),
                      egt::PixelFormat::xrgb8888,
                      egt::WindowHint::heo_overlay)
    {
        // Compute the byte stride (bytes per row) EGT expects for this format
        // and width. For xrgb8888 at 600px this is 600*4 = 2400 bytes.
        m_stride = egt::Surface::stride(egt::PixelFormat::xrgb8888, kVideoW);

        // Allocate the CPU-side frame buffer (stride * height), zero-filled
        // (a zeroed xrgb8888 buffer is opaque black).
        m_buf.resize(static_cast<size_t>(m_stride) * kVideoH, 0);

        // Wrap m_buf in an egt::Surface WITHOUT copying it. The Surface's
        // external-data constructor takes:
        //   data     -> pointer to our buffer
        //   release  -> called when the Surface is destroyed; a no-op here
        //               because m_buf (the vector) owns the storage and
        //               outlives the Surface.
        //   size     -> pixel dimensions
        //   format   -> xrgb8888
        //   stride   -> bytes per row (computed above)
        m_surface = std::make_shared<egt::Surface>(
            m_buf.data(), [](void*) {},
            egt::Size(kVideoW, kVideoH), egt::PixelFormat::xrgb8888, m_stride);

        // Build an egt::Image backed by that Surface. Drawing the Image blits
        // the Surface's current pixels; because the Image shares the Surface
        // (shared_ptr), updating the buffer + mark_dirty() is enough to show
        // a new frame -- no reallocation per frame.
        m_image   = std::make_unique<egt::Image>(m_surface);
    }

    // Copy one decoded BGRx frame into the plane buffer and request a repaint.
    //   src        : pointer to the source frame's pixels (from GStreamer)
    //   src_stride : the source's bytes-per-row (may differ from ours)
    void update_bgrx(const uint8_t* src, int src_stride)
    {
        const int rowbytes = kVideoW * 4;   // 4 bytes/pixel (BGRx)
        // Copy row by row because src_stride and m_stride can differ
        // (GStreamer may pad rows differently than EGT).
        for (int y = 0; y < kVideoH; ++y)
            std::memcpy(m_buf.data() + y * m_stride, src + y * src_stride, rowbytes);

        m_surface->mark_dirty();    // tell EGT the Surface pixels changed
        damage();                   // schedule a repaint of this window
    }

    // EGT calls draw() when the window is repainted. We simply stamp the
    // current Image at the top-left (0,0) of the plane.
    void draw(egt::Painter& painter, const egt::Rect&) override
    {
        if (m_image)
            // Painter::draw(Point) sets the origin and returns the Painter&,
            // then Painter::draw(Image) blits the image at that origin.
            painter.draw(egt::Point(0, 0)).draw(*m_image);
    }

private:
    std::shared_ptr<egt::Surface> m_surface; // wraps m_buf; shared with m_image
    std::unique_ptr<egt::Image>   m_image;   // drawable view of m_surface
    std::vector<uint8_t>          m_buf;      // CPU-side frame storage
    egt::DefaultDim               m_stride{0};// bytes per row of m_buf
};

/* --------------------------------------------------------------------------
 * one_shot_probe
 * --------------------------------------------------------------------------
 * A GStreamer pad probe used by the Capture feature. It is installed on the
 * valve's src pad and fires when the FIRST buffer passes through after the
 * valve opens. It immediately re-closes the valve (drop=TRUE) and removes
 * itself, so exactly one frame reaches the JPEG encoder per button press.
 *   user_data : the GstElement* valve (passed through from gst_pad_add_probe).
 * Returns GST_PAD_PROBE_REMOVE so the probe is uninstalled after this call.
 * -------------------------------------------------------------------------- */
GstPadProbeReturn one_shot_probe(GstPad*, GstPadProbeInfo*, gpointer user_data)
{
    auto* valve = static_cast<GstElement*>(user_data);
    g_object_set(valve, "drop", TRUE, nullptr);  // close the valve again
    return GST_PAD_PROBE_REMOVE;                 // uninstall this probe
}

/* ==========================================================================
 * sensor_thread_main  --  UDP :5001 "sensor detected" listener
 * --------------------------------------------------------------------------
 * Runs on its OWN thread (started from main) and does the ONE thing that is
 * safe to do off the UI thread: block in recvfrom() and flip an atomic flag.
 *
 * EGT (like most GUI toolkits) is NOT thread-safe, and the design note at the
 * top of this file relies on there being no cross-thread access to EGT/
 * GStreamer objects. We honour that here: this thread NEVER touches an EGT
 * widget. It only sets *detected; the UI-thread pump timer polls that flag and
 * updates the on-screen label, so all EGT access still happens on one thread.
 *
 *   running  : owned by main; set false at shutdown. A 1-second socket receive
 *              timeout (SO_RCVTIMEO) makes recvfrom() return periodically so
 *              the loop notices and exits without needing to close the socket
 *              from another thread.
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
    /* ----------------------------------------------------------------------
     * System log setup.
     *   openlog(ident, options, facility)
     *     ident   = "camera-gui" prefix on every syslog line
     *     LOG_PID = include the process id
     *     LOG_CONS= also print to the console if syslog is unavailable
     *     LOG_USER= generic user-level messages facility
     * -------------------------------------------------------------------- */
    openlog("camera-gui", LOG_PID | LOG_CONS, LOG_USER);
    LOGI("==== camera-gui starting (build %s %s) ====", __DATE__, __TIME__);

    /* ----------------------------------------------------------------------
     * If the user hasn't set GST_DEBUG in the environment, raise GStreamer's
     * default log threshold to WARNING so pipeline problems (missing plugins,
     * caps negotiation failures) are visible on stderr without extra flags.
     * g_getenv returns NULL when the variable is unset.
     * -------------------------------------------------------------------- */
    if (!g_getenv("GST_DEBUG")) {
        gst_debug_set_default_threshold(GST_LEVEL_WARNING);
        LOGI("GST_DEBUG not set; forcing GStreamer threshold to WARNING");
    }

    // Initialise GStreamer. Consumes any gst-specific argv options and must be
    // called before any other gst_* function.
    gst_init(&argc, &argv);
    LOGI("gst_init OK, version=%s", gst_version_string());

    // Create the EGT application object. This brings up the screen/KMS backend
    // and the event loop. Must exist before any window is created.
    egt::Application app(argc, argv);
    LOGI("egt::Application created (screen backend up)");

    /* ----------------------------------------------------------------------
     * Compositing model (Microchip LCDC)
     * ----------------------------------------------------------------------
     * Hardware OVERLAY planes are ALWAYS composited ABOVE the primary (base)
     * plane -- an opaque overlay hides whatever is on the primary plane below
     * it. So the UI can NOT live on the primary plane (a plain TopWindow): it
     * would be drawn, but hidden underneath the video overlay. That was the
     * "buttons not visible" bug.
     *
     * The fix (mirrors EGT's own examples/video/camera.cpp):
     *   - a root TopWindow owns the primary plane (kept black),
     *   - the VIDEO is one overlay plane (kVideoW x kVideoH, left-aligned),
     *   - the UI is a SECOND overlay plane (argb8888, transparent) added AFTER
     *     the video, so the LCDC composites it ABOVE the video. Its controls
     *     are children of it, so touch events route correctly.
     * -------------------------------------------------------------------- */
    constexpr int kPanelX = kVideoW;                 // 600 -- right column start
    constexpr int kPanelW = kWidth - kVideoW;        // 200 -- right column width

    // Root / primary plane. Fully covered by the two overlays above it.
    egt::TopWindow win;
    win.color(egt::Palette::ColorId::bg, egt::Palette::black);

    // ---- Video overlay (lower overlay plane), left-aligned kVideoW x kVideoH.
    VideoPlane video;
    LOGI("VideoPlane constructed (%dx%d @ left, xrgb8888, heo_overlay)", kVideoW, kVideoH);
    win.add(video);                     // add FIRST -> lower overlay plane
    video.show();                       // make the overlay plane visible
    LOGI("VideoPlane shown");

    // ---- UI overlay (upper overlay plane): full-screen, transparent so the
    // video shows through on the left; a dark control panel on the right.
    egt::Window ui(egt::Size(kWidth, kHeight), egt::PixelFormat::argb8888);
    ui.color(egt::Palette::ColorId::bg, egt::Palette::transparent);
    if (!ui.plane_window())             // no free hardware plane -> software blend
        ui.fill_flags(egt::Theme::FillFlag::blend);
    win.add(ui);                        // added AFTER video -> composited ABOVE it
    LOGI("UI overlay created (plane_window=%d)", ui.plane_window() ? 1 : 0);

    // Opaque dark panel filling the right column: a clean backdrop for the
    // controls (so they don't sit on transparent pixels where the video isn't).
    egt::Frame panel(egt::Rect(egt::Point(kPanelX, 0), egt::Size(kPanelW, kHeight)));
    panel.fill_flags(egt::Theme::FillFlag::solid);
    panel.color(egt::Palette::ColorId::bg, egt::Color(0x202020ff));
    ui.add(panel);

    // Capture button, horizontally centred in the right column.
    constexpr int kBtnW = 160, kBtnH = 60;
    egt::Button capture("Capture",
        egt::Rect(egt::Point(kPanelX + (kPanelW - kBtnW) / 2, 200),
                  egt::Size(kBtnW, kBtnH)));

    // Status label near the top of the right column, initially "Live".
    // Text is kept SHORT ("Live", "Capturing...", "Saved", "No camera feed") so
    // it fits the ~180px column; the full capture path is only logged, never
    // shown. (An earlier attempt to display the filename overflowed the label
    // and drew off the right edge of the screen.)
    egt::Label status("Live",
        egt::Rect(egt::Point(kPanelX + 10, 40),
                  egt::Size(kPanelW - 20, 40)));
    // White text on the dark panel.
    status.color(egt::Palette::ColorId::label_text, egt::Palette::white);
    ui.add(capture);                    // add widgets to the UI overlay
    ui.add(status);

    // "Sensor Detected" label, positioned just BELOW the Capture button (the
    // button spans y=200..260). Starts blank; the pump timer fills it in when
    // the UDP sensor thread reports a detection, then clears it a few seconds
    // later. Yellow text so it stands out from the white status label above.
    egt::Label sensor("",
        egt::Rect(egt::Point(kPanelX + 10, 280),
                  egt::Size(kPanelW - 20, 40)));
    sensor.color(egt::Palette::ColorId::label_text, egt::Palette::yellow);
    ui.add(sensor);

    /* ======================================================================
     * GStreamer pipeline construction
     * ----------------------------------------------------------------------
     * The pipeline has a source branch feeding a "tee", which fans out to:
     *   - a DISPLAY branch  (always present) ending in an appsink, and
     *   - a CAPTURE branch  (optional)       ending in a JPEG multifilesink.
     * We build the pipeline as a text description and hand it to
     * gst_parse_launch(), the same syntax as `gst-launch-1.0`.
     * ====================================================================== */

    // ---- RTP caps for the RAW (uncompressed) source (the SAM9X75) ----------
    // These caps describe the incoming RTP payload so udpsrc/the depayloader
    // know how to interpret packets (RTP carries no self-describing header for
    // raw video). media/clock-rate/encoding-name/sampling/depth/width/height/
    // payload MUST match what the sender transmits exactly.
    const std::string raw_caps =
        "application/x-rtp,media=(string)video,clock-rate=(int)90000,"
        "encoding-name=(string)RAW,sampling=(string)YCbCr-4:2:2,depth=(string)8,"
        "width=(string)" + std::to_string(kWidth) +
        ",height=(string)" + std::to_string(kHeight) + ",payload=(int)96";

    // ---- RTP caps for the H.264 source (e.g. webcam_stream.sh) -------------
    // encoding-name=H264 selects the H.264 depayloader path. clock-rate 90000
    // is the standard for video; payload 96 is the dynamic PT used by the
    // sender (rtph264pay pt=96).
    const std::string h264_caps =
        "application/x-rtp,media=(string)video,clock-rate=(int)90000,"
        "encoding-name=(string)H264,payload=(int)96";

    // ---- Choose the source at runtime via environment variables -----------
    //   CAMERA_GUI_TESTSRC -> local test pattern (no network)
    //   CAMERA_GUI_H264    -> H.264 RTP receive
    //   (neither)          -> raw YCbCr-4:2:2 RTP receive
    const bool use_testsrc = (g_getenv("CAMERA_GUI_TESTSRC") != nullptr);
    const bool use_h264    = (g_getenv("CAMERA_GUI_H264")    != nullptr);

    // ---- Jitter-buffer latency (ms) ---------------------------------------
    // The rtpjitterbuffer holds incoming packets for this long to absorb
    // network jitter and reorder packets before depayloading. Too small ->
    // packets arrive "late" and get dropped (corrupt frames); too large ->
    // more end-to-end delay. Default 200ms; override with CAMERA_GUI_LATENCY.
    const char* lat_env = g_getenv("CAMERA_GUI_LATENCY");
    const int latency = lat_env ? atoi(lat_env) : 200;

    std::string source;
    if (use_testsrc) {
        // videotestsrc is-live=true : generate a live SMPTE-style test pattern.
        // Followed by explicit width/height caps, then the tee. Useful to prove
        // the display path without any network/source.
        source = "videotestsrc is-live=true ! video/x-raw,width=" +
                 std::to_string(kWidth) + ",height=" + std::to_string(kHeight) +
                 " ! tee name=t ";
        LOGW("CAMERA_GUI_TESTSRC set -> using videotestsrc instead of udpsrc");
    } else if (use_h264) {
        // Verify the software H.264 decoder is actually installed (provided by
        // gst1-libav). If missing, the pipeline would fail to build.
        if (!gst_element_factory_find("avdec_h264"))
            LOGE("avdec_h264 factory NOT found -- is gst1-libav installed? decode will fail");
        LOGI("H.264 receive mode, rtpjitterbuffer latency=%d ms", latency);

        // H.264 receive branch, element by element:
        //   udpsrc port=5000            : receive UDP packets on port 5000
        //     buffer-size=2097152       : 2MB kernel socket buffer -- absorbs
        //                                 bursts without kernel packet drops;
        //                                 stays near-empty under realtime decode
        //                                 so it does not add latency.
        //     caps="...H264..."         : tell udpsrc/depay how to read the RTP
        //   rtpjitterbuffer             : reorder/dejitter incoming RTP
        //     latency=<ms>              : buffering window (see above)
        //     drop-on-latency=true      : drop packets that exceed the window
        //                                 instead of buffering them (keeps live)
        //   rtph264depay                : reassemble RTP payloads into H.264
        //   avdec_h264                  : software H.264 decoder -> raw video
        //     max-threads=1             : disable frame-threading, which would
        //                                 otherwise delay output by several frames
        //     output-corrupt=false      : DROP frames that can't be fully
        //                                 decoded (packet loss) instead of
        //                                 emitting macroblock noise / white smears
        //   tee name=t                  : fan-out point for display/capture
        source =
            "udpsrc port=" + std::to_string(kPort) + " buffer-size=2097152 caps=\"" + h264_caps + "\" "
            "! rtpjitterbuffer latency=" + std::to_string(latency) + " drop-on-latency=true "
            "! rtph264depay ! avdec_h264 max-threads=1 output-corrupt=false ! tee name=t ";
    } else {
        LOGI("RAW (YCbCr-4:2:2) receive mode, rtpjitterbuffer latency=%d ms", latency);
        // Raw receive branch:
        //   udpsrc buffer-size=8388608  : 8MB socket buffer (raw video is huge,
        //                                 needs more headroom than H.264)
        //   rtpjitterbuffer ...         : same dejitter/drop-on-latency behavior
        //   rtpvrawdepay                : reassemble RTP into raw video frames
        //   tee name=t                  : fan-out point
        source =
            "udpsrc port=" + std::to_string(kPort) + " buffer-size=8388608 caps=\"" + raw_caps + "\" "
            "! rtpjitterbuffer latency=" + std::to_string(latency) + " drop-on-latency=true "
            "! rtpvrawdepay ! tee name=t ";
    }

    // ---- DISPLAY branch (always present) ----------------------------------
    //   t.                              : take a branch off the tee
    //   queue leaky=downstream          : decouple threads; drop the OLDEST
    //     max-size-buffers=2            : buffered frames when full so we always
    //     max-size-time=0               : show the newest frame (low latency).
    //     max-size-bytes=0              : (time/bytes limits disabled; only the
    //                                     2-buffer limit applies)
    //   videoconvertscale               : convert to BGRx AND scale any source
    //                                     resolution (e.g. 640x480) to 800x480
    //                                     in one element
    //   video/x-raw,format=BGRx,WxH     : force the output format/size
    //   appsink name=disp               : hand decoded frames to the app
    //     emit-signals=false            : we PULL frames (no signal callbacks)
    //     sync=false                    : render ASAP, don't sync to the clock
    //     max-buffers=1 drop=true       : keep only the newest 1 frame, drop
    //                                     older ones (prevents backlog/latency)
    std::string desc = source +
        "t. ! queue leaky=downstream max-size-buffers=2 max-size-time=0 max-size-bytes=0 "
        "! videoconvertscale ! video/x-raw,format=BGRx,"
             "width=" + std::to_string(kVideoW) + ",height=" + std::to_string(kVideoH) + " "
        "! appsink name=disp emit-signals=false sync=false max-buffers=1 drop=true ";

    // Ensure the capture directory exists (created on first run). kOutDir is
    // <home>/camera-gui-capture; a single mkdir suffices because $HOME exists.
    if (::mkdir(kOutDir.c_str(), 0755) != 0 && errno != EEXIST)
        LOGW("could not create capture dir %s (%s)", kOutDir.c_str(), std::strerror(errno));
    else
        LOGI("capture dir: %s", kOutDir.c_str());

    // ---- CAPTURE branch (optional; requires the multifilesink element) -----
    // gst_element_factory_find returns non-NULL only if the plugin is present.
    // We append the capture branch ONLY when multifilesink exists, so a missing
    // plugin can never break the (essential) display branch.
    const bool have_multifilesink =
        gst_element_factory_find("multifilesink") != nullptr;
    if (have_multifilesink) {
        // Capture branch:
        //   t. ! queue                  : second tee branch, decoupled
        //   valve name=capgate drop=true: normally DROPS everything (closed);
        //                                 opened for one frame on button press
        //   videoconvert                : convert raw video to jpegenc's input
        //   jpegenc quality=90          : encode a single JPEG
        //   multifilesink               : write each buffer to a numbered file
        //     location=.../cap_%05d.jpg : filename pattern (cap_00000.jpg, ...)
        //     post-messages=true        : post a bus message per file written
        //                                 (used to update the status label)
        desc +=
            "t. ! queue ! valve name=capgate drop=true ! videoconvert ! jpegenc quality=90 "
            "! multifilesink name=capsink location=" + std::string(kOutDir) + "/cap_%05d.jpg "
                 // async=false: the valve is CLOSED by default, so this sink never
                 // receives a preroll buffer. Without async=false a GstBaseSink
                 // holds the whole pipeline in PAUSED (ASYNC never completes) and
                 // the display branch never advances to PLAYING -> blank screen.
                 // sync=false: capture writes as fast as frames arrive, no clock wait.
                 "post-messages=true async=false sync=false";
    } else {
        LOGW("multifilesink not installed -> capture disabled "
             "(enable BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_MULTIFILE in buildroot)");
    }

    LOGI("pipeline desc: %s", desc.c_str());

    /* ----------------------------------------------------------------------
     * Build the pipeline from the text description.
     *   gst_parse_launch(desc, &err) parses the string and returns a
     *   GstElement* (the top-level pipeline), or NULL on a fatal error.
     *   `err` may also be set for RECOVERABLE warnings even on success.
     * -------------------------------------------------------------------- */
    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(desc.c_str(), &err);
    if (!pipeline) {
        LOGE("gst_parse_launch FAILED: %s", err ? err->message : "unknown");
        if (err) g_error_free(err);
        return 1;                       // cannot continue without a pipeline
    }
    if (err) {                          // non-fatal: log and clear the warning
        LOGW("gst_parse_launch warning: %s", err->message);
        g_error_free(err);
        err = nullptr;
    }
    LOGI("gst_parse_launch OK");

    /* ----------------------------------------------------------------------
     * Look up the named elements we need to interact with at runtime, and get
     * the pipeline's message bus.
     *   gst_bin_get_by_name : find an element by its name= in the description.
     *   gst_element_get_bus : the bus carries async messages (errors, EOS,
     *                         state changes, element messages).
     * (These return NULL if not found; we log accordingly.)
     * -------------------------------------------------------------------- */
    GstElement* appsink = gst_bin_get_by_name(GST_BIN(pipeline), "disp");
    GstElement* valve   = gst_bin_get_by_name(GST_BIN(pipeline), "capgate");
    GstElement* capsink = gst_bin_get_by_name(GST_BIN(pipeline), "capsink");
    GstBus*     bus     = gst_element_get_bus(pipeline);
    if (!appsink) LOGE("appsink 'disp' NOT found in pipeline");
    if (!valve && have_multifilesink) LOGW("valve 'capgate' NOT found in pipeline");
    if (!bus)     LOGE("could not get pipeline bus");
    LOGI("elements: appsink=%p valve=%p capsink=%p bus=%p",
         (void*)appsink, (void*)valve, (void*)capsink, (void*)bus);

    /* ----------------------------------------------------------------------
     * Start the pipeline.
     *   gst_element_set_state(..., GST_STATE_PLAYING) begins streaming.
     *   For live/network sources this typically returns ASYNC (the state
     *   change completes in the background once data flows). We then block
     *   briefly with gst_element_get_state() to log the REAL outcome.
     * -------------------------------------------------------------------- */
    GstStateChangeReturn scr = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    LOGI("set_state(PLAYING) returned %s", gst_element_state_change_return_get_name(scr));
    if (scr == GST_STATE_CHANGE_FAILURE) {
        LOGE("pipeline failed to start playing (check UDP source / plugins)");
    } else {
        GstState st = GST_STATE_NULL, pend = GST_STATE_NULL;
        // Wait up to 3 seconds for the state change to settle.
        GstStateChangeReturn r =
            gst_element_get_state(pipeline, &st, &pend, 3 * GST_SECOND);
        LOGI("state after 3s: cur=%s pending=%s (get_state=%s)",
             gst_element_state_get_name(st),
             gst_element_state_get_name(pend),
             gst_element_state_change_return_get_name(r));
    }

    /* ----------------------------------------------------------------------
     * UI-pump counters / deadlines (shared_ptr so they persist across timer
     * callbacks). Declared here, BEFORE do_capture, so the capture action can
     * arm the status-revert deadline itself.
     *   frame_count        : decoded frames blitted so far.
     *   tick_count         : pump ticks elapsed (~33ms each).
     *   last_frame_tick    : tick at which the most recent frame was blitted;
     *                        used to tell whether a live feed is currently
     *                        arriving (see do_capture's "no feed" guard).
     *   revert_tick        : tick at which the status label returns to "Live";
     *                        0 = nothing pending.
     *   sensor_revert_tick : tick at which the "Sensor Detected" label clears.
     * -------------------------------------------------------------------- */
    auto frame_count = std::make_shared<unsigned long>(0);
    auto tick_count  = std::make_shared<unsigned long>(0);
    auto last_frame_tick = std::make_shared<unsigned long>(0);
    auto revert_tick = std::make_shared<unsigned long>(0);
    auto sensor_revert_tick = std::make_shared<unsigned long>(0);

    /* ----------------------------------------------------------------------
     * Capture action (shared by the Capture button and the sensor trigger).
     * Verifies the valve exists and the SD dir is writable, then installs a
     * one-shot pad probe on the valve's src pad and opens the valve. The probe
     * lets exactly one buffer through, then re-closes the valve -> one JPEG.
     *
     * Defined as a lambda so BOTH the button's on_click handler AND the UDP
     * sensor path (in the pump timer) can invoke the identical capture/save
     * sequence. It MUST only be called on the UI thread (both callers are),
     * because it touches GStreamer/EGT objects with no locking.
     *   'reason' is a short tag ("button"/"sensor") used only for logging.
     * -------------------------------------------------------------------- */
    auto do_capture = [&](const char* reason) {
        LOGI("capture requested (%s)", reason);
        if (!valve) {                   // capture branch wasn't built
            LOGW("capture unavailable (multifilesink not installed)");
            status.text("Capture unavailable");
            return;
        }
        // Refuse to capture when no live feed is arriving: at startup before the
        // first frame (frame_count == 0), or after the stream stalls/drops (no
        // frame within kFeedStaleTicks). Grabbing here would save an empty or
        // stale JPEG, so we surface "No camera feed" and leave the pipeline
        // untouched instead of flipping the status to "Capturing...".
        if (*frame_count == 0 ||
            (*tick_count - *last_frame_tick) > kFeedStaleTicks) {
            LOGW("capture refused (%s): no live feed (frames=%lu, idle=%lu ticks)",
                 reason, *frame_count,
                 *frame_count ? (*tick_count - *last_frame_tick) : 0);
            status.text("No camera feed");
            // Show the notice briefly, then hand back to "Live".
            *revert_tick = *tick_count + kStatusRevertTicks;
            return;
        }
        if (::access(kOutDir.c_str(), W_OK) != 0) {   // POSIX: is the dir writable?
            LOGW("capture dir %s not writable", kOutDir.c_str());
            status.text("Capture dir not writable");
            return;
        }
        // Build a per-capture filename carrying the wall-clock timestamp in
        // addition to multifilesink's running %05d index, e.g.
        //   cap_20260804_143512_00000.jpg
        // (Changing 'location' does not reset the index, so the counter from
        // the existing format is preserved across captures.)
        if (capsink) {
            char ts[32] = "unknown";
            const std::time_t now = std::time(nullptr);
            if (const std::tm* tmv = std::localtime(&now))
                std::strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", tmv);
            const std::string loc = kOutDir + "/cap_" + ts + "_%05d.jpg";
            g_object_set(capsink, "location", loc.c_str(), nullptr);
        }
        // Get the valve's source pad and attach the one-shot buffer probe.
        GstPad* src = gst_element_get_static_pad(valve, "src");
        gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, one_shot_probe, valve, nullptr);
        gst_object_unref(src);          // release our ref on the pad
        // Open the valve; the probe will re-close it after the first buffer.
        g_object_set(valve, "drop", FALSE, nullptr);
        status.text("Capturing...");
        // Arm the status-revert deadline HERE, at capture time. This guarantees
        // the label returns to "Live" ~3s later even if the multifilesink
        // "file written" bus message is missed or never arrives -- previously
        // the revert was only armed inside that message handler, so a missed
        // message left the status stuck (never returning to "Live").
        *revert_tick = *tick_count + kStatusRevertTicks;
    };

    // The Capture button simply invokes the shared capture action.
    capture.on_click([&](egt::Event&) {
        do_capture("button");
    });

    /* ----------------------------------------------------------------------
     * Per-tick pump (runs on the UI thread every 33ms ~= 30Hz).
     * Two responsibilities each tick:
     *   1. Pull the newest decoded frame from the appsink and blit it.
     *   2. Drain the GStreamer bus (errors, warnings, state changes, EOS,
     *      and the capture "file written" element messages).
     *
     * The counters/deadlines it uses (frame_count, tick_count, revert_tick,
     * sensor_revert_tick) are declared above, before do_capture.
     * -------------------------------------------------------------------- */

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

    egt::PeriodicTimer pump(std::chrono::milliseconds(33));
    pump.on_timeout([&, frame_count, tick_count, last_frame_tick, revert_tick, sensor_revert_tick]() {
        ++*tick_count;

        // Restore the "Live" label a few seconds after a capture confirmation,
        // so the "Saved ..." text shows briefly and then hands back to live feed.
        if (*revert_tick != 0 && *tick_count >= *revert_tick) {
            status.text("Live");
            *revert_tick = 0;
        }

        // Sensor notifier: consume the atomic flag the UDP thread set (if any).
        // On a detection we (1) show "Sensor Detected" below the Capture button
        // and (2) trigger the SAME capture/save action as the button. Running
        // it here (on the UI thread, via the pump) keeps all GStreamer/EGT
        // access single-threaded -- the UDP thread never touches these objects.
        if (sensor_detected.exchange(false)) {
            sensor.text("Sensor Detected");
            *sensor_revert_tick = *tick_count + kSensorRevertTicks;
            do_capture("sensor");       // capture + save a frame on detection
        }
        // Clear the label once its display window elapses (matches the capture
        // status's briefly-then-revert behaviour).
        if (*sensor_revert_tick != 0 && *tick_count >= *sensor_revert_tick) {
            sensor.text("");
            *sensor_revert_tick = 0;
        }
        // gst_app_sink_try_pull_sample(sink, 0): non-blocking (0 timeout) pull
        // of the most recent sample. Returns NULL if none is ready this tick.
        if (GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), 0)) {
            GstBuffer* buf = gst_sample_get_buffer(s);   // the pixel data
            GstVideoInfo info;
            // Parse the sample's caps into a GstVideoInfo (gives width/height
            // and per-plane strides for the frame).
            if (gst_video_info_from_caps(&info, gst_sample_get_caps(s))) {
                GstMapInfo m;
                // Map the buffer into CPU-readable memory.
                if (gst_buffer_map(buf, &m, GST_MAP_READ)) {
                    // Blit the frame onto the overlay plane. Plane-0 stride is
                    // the source's bytes-per-row.
                    video.update_bgrx(m.data, GST_VIDEO_INFO_PLANE_STRIDE(&info, 0));
                    gst_buffer_unmap(buf, &m);           // always unmap
                    ++*frame_count;
                    *last_frame_tick = *tick_count;      // mark the feed "live"
                    if (*frame_count == 1)
                        LOGI("FIRST video frame received & blitted (%dx%d stride=%d)",
                             GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info),
                             GST_VIDEO_INFO_PLANE_STRIDE(&info, 0));
                    else if (*frame_count % 150 == 0)    // ~ every 5s at 30fps
                        LOGI("frames=%lu (streaming OK)", *frame_count);
                } else {
                    LOGW("gst_buffer_map failed");
                }
            } else {
                LOGW("gst_video_info_from_caps failed");
            }
            gst_sample_unref(s);         // release the sample
        }

        // Diagnostic: if ~5s pass with zero frames, warn that no video arrived.
        if (*tick_count % 150 == 0 && *frame_count == 0)
            LOGW("no frames after %lu ticks -- is the sender streaming to UDP port %d?",
                 *tick_count, kPort);

        // Drain ALL pending bus messages this tick (non-blocking pop).
        while (GstMessage* msg = gst_bus_pop(bus)) {
            switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ELEMENT: {          // e.g. multifilesink "file written"
                const GstStructure* st = gst_message_get_structure(msg);
                const char* fn = st ? gst_structure_get_string(st, "filename") : nullptr;
                if (fn) {
                    // Show a SHORT confirmation only. The filename (basename ~29
                    // chars, full path ~55) does not fit the ~180px status label:
                    // centered it rendered blank, left-aligned it ran off the
                    // right edge of the screen. So we show just "Saved" and log
                    // the full path for the record.
                    status.text("Saved");
                    LOGI("captured -> %s", fn);
                    // Keep the "Saved ..." confirmation up for the full window,
                    // then revert to "Live". (The deadline is also armed at
                    // capture time in do_capture, so a missed message still
                    // reverts.)
                    *revert_tick = *tick_count + kStatusRevertTicks;
                }
                break;
            }
            case GST_MESSAGE_ERROR: {            // fatal pipeline error
                GError* e = nullptr; gchar* dbg = nullptr;
                gst_message_parse_error(msg, &e, &dbg);
                LOGE("pipeline ERROR from %s: %s | debug: %s",
                     GST_OBJECT_NAME(msg->src), e ? e->message : "?", dbg ? dbg : "none");
                status.text(std::string("Stream error: ") + (e ? e->message : "?"));
                if (e) g_error_free(e);
                g_free(dbg);
                break;
            }
            case GST_MESSAGE_WARNING: {          // non-fatal pipeline warning
                GError* e = nullptr; gchar* dbg = nullptr;
                gst_message_parse_warning(msg, &e, &dbg);
                LOGW("pipeline WARNING from %s: %s | debug: %s",
                     GST_OBJECT_NAME(msg->src), e ? e->message : "?", dbg ? dbg : "none");
                if (e) g_error_free(e);
                g_free(dbg);
                break;
            }
            case GST_MESSAGE_STATE_CHANGED: {    // log top-level state changes
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                    GstState o, n, p;
                    gst_message_parse_state_changed(msg, &o, &n, &p);
                    LOGI("pipeline state: %s -> %s",
                         gst_element_state_get_name(o), gst_element_state_get_name(n));
                }
                break;
            }
            case GST_MESSAGE_EOS:                // end of stream
                LOGW("pipeline EOS (stream ended)");
                break;
            default: break;
            }
            gst_message_unref(msg);              // release each message
        }
    });
    pump.start();                                // begin the 33ms timer
    LOGI("pump timer started (33ms); entering app.run()");

    win.show();                                  // show root (primary plane)
    ui.show();                                    // show the UI overlay (above video)
    int rc = app.run();                          // enter EGT's event loop (blocks)
    LOGI("app.run() returned %d; shutting down", rc);

    /* ----------------------------------------------------------------------
     * Stop the UDP sensor thread first: clear its run flag and join. The
     * thread's 1s socket receive timeout guarantees it observes the flag and
     * returns within ~1s, so this join does not hang.
     * -------------------------------------------------------------------- */
    sensor_running.store(false);
    if (sensor_thread.joinable())
        sensor_thread.join();
    LOGI("sensor thread joined");

    /* ----------------------------------------------------------------------
     * Shutdown / cleanup.
     *   - Stop the pipeline (GST_STATE_NULL frees element resources).
     *   - Drop our references to the objects we looked up / created.
     *   - Close the system log.
     * -------------------------------------------------------------------- */
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus)     gst_object_unref(bus);
    if (valve)   gst_object_unref(valve);
    if (capsink) gst_object_unref(capsink);
    if (appsink) gst_object_unref(appsink);
    gst_object_unref(pipeline);
    LOGI("==== camera-gui exit rc=%d ====", rc);
    closelog();
    return rc;
}
