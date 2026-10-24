#pragma once
// Audit trail with screenshots, desktop side (fairyfly-gateway docs/AUDIT_TRAIL.md). libffl's agent does the
// protocol (caps, policy, req.trail, step/shot frames, limits); fairyfly implements ffy::link::Recorder here:
//   - screenshots of the SAP GUI window the call used, before and after the step (PNG, at most 1920 px wide and
//     4 MiB), with password and bank fields filled solid before encoding; a screenshot whose sensitive field cannot
//     be located is dropped, never sent unblurred;
//   - field changes old -> new for steps that change SAP (gui_element_fill, tools annotated destructiveHint), masked
//     with libffl's proto::password_field / proto::bank_field (the one list shared with the gateway);
//   - one short description per step;
//   - the notice for the person at the desktop (console line, status file for the tray) while the trail is on.
// Portable: SAP GUI is reached only through ScreenSource (COM implementation in trail_screen_source.h, fakes in the
// unit tests), and every ScreenSource call runs on the main (COM) thread through `run_on_main`.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "link_agent.hpp"  // libffl: Recorder, StepRequest, StepRecord
#include "proto.hpp"       // libffl: TrailPolicy, password_field, bank_field

namespace fairyfly::link::trail {

/// A rectangle in pixels: screen coordinates (ScreenLeft/ScreenTop/Width/Height) or image coordinates.
struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
};

/// One element of the captured window, as SAP GUI scripting reports it.
struct Field {
    std::string id;       ///< full id, e.g. wnd[0]/usr/txtLFBK-BANKN[2,0]
    std::string type;     ///< GuiTextField, GuiPasswordField, GuiShell, ...
    std::string name;     ///< SAP field name (Name), e.g. LFBK-BANKN
    std::string label;    ///< text on the screen next to it (LeftLabel), or the tooltip, or the column title
    std::string tooltip;
    std::string value;    ///< Text (never filled for password fields)
    bool input = false;   ///< a field a user types into (text, combo, check box, radio button)
    std::optional<Rect> rect;            ///< screen rectangle; nullopt when it could not be read
    std::vector<std::string> columns;    ///< grids (GuiShell GridView) and column trees: technical column names
    /// What it shows could not be read (a control container whose content the tree does not list and the walk could
    /// not reach, a grid whose columns could not be read): blurred as a whole, whatever the blur setting.
    bool opaque = false;
};

/// What the window looked like at one moment. `complete` is false when the elements could not be enumerated: a
/// screenshot of such a screen is never sent (its sensitive fields are unknown).
struct ScreenState {
    bool complete = false;
    std::string window_id;   ///< wnd[0], wnd[1] (a popup), ...
    std::string title;       ///< window title
    std::string transaction; ///< current transaction code
    std::optional<Rect> window;  ///< screen rectangle of the captured window
    std::vector<Field> fields;
};

/// 8-bit RGB pixels, row-major, 3 bytes per pixel.
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgb;
    bool valid() const { return width > 0 && height > 0 && rgb.size() == size_t(width) * size_t(height) * 3; }
};

/// SAP GUI as the recorder sees it. Every method is called on the main (COM) thread only.
class ScreenSource {
public:
    virtual ~ScreenSource() = default;
    /// The active window of the session the link's calls use and its elements, with the screen rectangles of the
    /// elements for which `needs_rect` is true (the ones to blur). Never throws: an error yields complete == false.
    virtual ScreenState state(const std::function<bool(const Field&)>& needs_rect) = 0;
    /// The active window as PNG bytes (GuiFrameWindow.HardCopyToMemory), empty when it cannot be captured or when
    /// the active window is no longer `window_id` (a popup opened after state() was read).
    virtual std::string capture_png(const std::string& window_id) = 0;
    /// The current value of one element (gui_element_fill's target); a grid cell when row/column are given.
    /// nullopt when it cannot be read.
    virtual std::optional<std::string> value(const std::string& element_id, std::optional<int> row, const std::string& column) = 0;
};

// ---- the field list (one place; libffl holds the actual list) -----------------------------------------------------

/// A password field: GuiPasswordField, or libffl's proto::password_field on its id, name, label or tooltip.
bool is_password(const Field& f);
/// A bank field: libffl's proto::bank_field on its id, name, label, tooltip or (grids) any column.
bool is_bank(const Field& f);
/// The rule for screenshots: password fields always, bank fields while `blur` is on.
bool must_blur(const Field& f, bool blur);

// ---- blurring -----------------------------------------------------------------------------------------------------

struct BlurPlan {
    bool drop = false;        ///< the screenshot must not be sent (a sensitive field could not be located)
    std::string reason;       ///< why it is dropped (constant text, never screen content)
    std::vector<Rect> rects;  ///< image rectangles to fill (padded, clamped to the image)
    int blurred = 0;          ///< fields covered
};
/// Pixels added around every rectangle: SAP GUI draws a field's frame and focus ring just outside it.
inline constexpr int kBlurPadding = 3;
/// The image must have the window's proportions: scaled by the image width, the window height may differ from the
/// image height by at most this many pixels, otherwise element positions cannot be mapped onto it reliably (and a
/// screen with sensitive fields is dropped). The scale itself must lie in [0.5, 4] (display scaling).
inline constexpr int kAspectTolerancePx = 2;

/// Maps the sensitive fields of `screen` onto an image of `image_width` x `image_height` pixels captured from
/// screen.window: image = (screen - window origin) * (image size / window size).
BlurPlan plan_blur(const ScreenState& screen, int image_width, int image_height, bool blur);
/// Fills the rectangles with a solid colour (not reversible). Pixels outside them stay as they are.
void fill_rects(Image& image, const std::vector<Rect>& rects);
/// Area-average downscale to `width` (keeps the aspect ratio). Returns the image unchanged if it is not wider.
Image scale_to_width(const Image& image, int width);

inline constexpr int kMaxShotWidth = 1920;

// ---- PNG (libpng, in memory: no unblurred pixel ever reaches a file) ------------------------------------------------

/// nullopt when the bytes are not a PNG libpng can read (or larger than 16384 px on a side).
std::optional<Image> decode_png(const std::string& bytes);
/// Empty on failure.
std::string encode_png(const Image& image);

/// The whole screenshot pipeline: decode, plan, fill, scale to <= kMaxShotWidth, encode, shrink until the PNG is at
/// most `max_bytes`. nullopt (with `plan.reason`) when it must not be sent.
struct Shot {
    std::string png;
    int blurred = 0;
};
std::optional<Shot> make_shot(const std::string& raw_png, const ScreenState& screen, bool blur, std::string* why = nullptr,
                              size_t max_bytes = ffy::proto::kShotMaxBytes);

// ---- field changes and descriptions ---------------------------------------------------------------------------------

/// The value of a field in a step record: "••••" for password fields, and for bank fields while `blur` is on.
std::string masked_value(const Field& f, const std::string& value, bool blur);
/// Input fields whose value differs between the two states (same id), at most kStepMaxChanges, masked.
std::vector<ffy::link::FieldChange> diff_fields(const ScreenState& before, const ScreenState& after, bool blur);

/// One short line for the step: what the assistant did, never a value it typed.
std::string describe(const std::string& tool, const nlohmann::json& args, const ScreenState* before, const ScreenState* after,
                     const std::vector<ffy::link::FieldChange>& changes, bool failed);

// ---- the notice -----------------------------------------------------------------------------------------------------

/// The console line while the trail is on, e.g. "Audit trail ON: your administrator records the steps assistants
/// take on this desktop, with screenshots (kept 90 days)".
std::string notice_line(const ffy::proto::TrailPolicy& p);
/// `fairyfly link status`: "on since 2026-10-24 (screenshots: every step; kept 90 days; blurring on)" or "off".
std::string status_text(const nlohmann::json& trail);

// ---- the recorder ---------------------------------------------------------------------------------------------------

struct RecorderOptions {
    std::shared_ptr<ScreenSource> source;
    /// Runs `fn` on the main (COM) thread and waits for it; false when it did not run (timeout, stopping).
    std::function<bool(std::function<void()> fn)> run_on_main;
    /// Called on every policy change (the notice): console line and status file.
    std::function<void(const ffy::proto::TrailPolicy& p)> on_policy;
};

class TrailRecorder : public ffy::link::Recorder {
public:
    explicit TrailRecorder(RecorderOptions options);

    void policy(const ffy::proto::TrailPolicy& p) override;
    bool can_blur() const override { return true; }
    void before(const ffy::link::StepRequest& s) override;
    ffy::link::StepRecord after(const ffy::link::StepRequest& s, const nlohmann::json& result) override;

    /// Tools that change SAP besides gui_element_fill (destructiveHint in the catalog). Thread-safe.
    void set_change_tools(std::set<std::string> tools);
    bool changes_sap(const std::string& tool) const;
    /// The policy last received (for the status file).
    ffy::proto::TrailPolicy current_policy() const;

private:
    struct Snapshot {
        ScreenState state;
        std::string raw_png;
        std::optional<std::string> target_value;  ///< gui_element_fill: the target's value
    };
    std::optional<Snapshot> snapshot(const ffy::link::StepRequest& s, bool shot);

    RecorderOptions o_;
    mutable std::mutex mu_;
    ffy::proto::TrailPolicy policy_;
    std::set<std::string> change_tools_;
    struct Pending {
        ScreenState state;
        std::optional<Shot> shot;
        std::optional<std::string> target_value;
    };
    std::map<std::string, Pending> pending_;  ///< request id -> the "before" of a step whose after() is still to come
};

/// "wnd[0]/usr/txtX" from "/app/con[0]/ses[0]/wnd[0]/usr/txtX" (the form gui_screen_read reports and the gateway
/// shows); other ids unchanged.
std::string relative_id(const std::string& id);

/// The element a gui_element_fill call targets (args element | id | element_id), and its grid cell.
struct FillTarget {
    std::string element;
    std::optional<int> row;
    std::string column;
};
std::optional<FillTarget> fill_target(const nlohmann::json& args);

}  // namespace fairyfly::link::trail
