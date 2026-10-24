// Audit trail with screenshots, desktop side (fairyfly-gateway docs/AUDIT_TRAIL.md): the recorder fairyfly hands
// to libffl's agent. SAP GUI is replaced by a fake ScreenSource (the COM boundary); everything else is real: the
// PNG codec, the blurring, the masking list (libffl's proto::password_field / bank_field), MainThreadTools and
// libffl's Agent::serve with its step and shot frames. Nothing here touches %LOCALAPPDATA%\fairyfly\link.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "include/link/link_control.h"
#include "include/link/link_runner.h"
#include "include/link/main_thread_tools.h"
#include "include/link/trail_recorder.h"
#include "include/link/trail_screen_source.h"
#include "assertion.hpp"
#include "crypto.hpp"
#include "link_agent.hpp"
#include "signer.hpp"
#include "util.hpp"

#ifdef _WIN32
#include "include/com/wrapper.h"
#endif

namespace fs = std::filesystem;
using namespace fairyfly::link;
using namespace fairyfly::link::trail;
using nlohmann::json;

namespace {

constexpr std::uint8_t kFill = 0x60;

/// A white image with a coloured stripe in every row, so "unchanged" means something.
Image test_image(int w, int h) {
    Image img;
    img.width = w;
    img.height = h;
    img.rgb.assign(size_t(w) * size_t(h) * 3, 255);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            auto* p = img.rgb.data() + (size_t(y) * size_t(w) + size_t(x)) * 3;
            if ((x / 7 + y / 5) % 3 == 0) {
                p[0] = std::uint8_t(x % 251);
                p[1] = std::uint8_t(y % 241);
                p[2] = 200;
            }
        }
    return img;
}
/// Flat 40 x 24 px blocks, like a real screen's large flat areas: smaller when scaled down.
Image block_image(int w, int h) {
    Image img;
    img.width = w;
    img.height = h;
    img.rgb.resize(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            auto* p = img.rgb.data() + (size_t(y) * size_t(w) + size_t(x)) * 3;
            const unsigned v = (unsigned(x / 40) * 2654435761u) ^ (unsigned(y / 24) * 40503u);
            p[0] = std::uint8_t(v);
            p[1] = std::uint8_t(v >> 8);
            p[2] = std::uint8_t(v >> 16);
        }
    return img;
}
const std::uint8_t* px(const Image& img, int x, int y) { return img.rgb.data() + (size_t(y) * size_t(img.width) + size_t(x)) * 3; }
bool is_fill(const Image& img, int x, int y) {
    auto p = px(img, x, y);
    return p[0] == kFill && p[1] == kFill && p[2] == kFill;
}
bool same(const Image& a, const Image& b, int x, int y) {
    auto p = px(a, x, y), q = px(b, x, y);
    return p[0] == q[0] && p[1] == q[1] && p[2] == q[2];
}

Field field(std::string id, std::string type, std::optional<Rect> rect, std::string label = "", std::string value = "") {
    Field f;
    f.id = std::move(id);
    f.type = std::move(type);
    f.rect = rect;
    f.label = std::move(label);
    f.value = std::move(value);
    f.input = f.type == "GuiTextField" || f.type == "GuiCTextField" || f.type == "GuiPasswordField" || f.type == "GuiComboBox";
    auto dash = f.id.rfind('/');
    std::string leaf = dash == std::string::npos ? f.id : f.id.substr(dash + 1);
    size_t i = 0;
    while (i < leaf.size() && std::islower(static_cast<unsigned char>(leaf[i]))) i++;  // txt, ctxt, pwd, ...
    f.name = leaf.substr(i);
    return f;
}

/// A SAP logon-like screen: window at (100,50) 400x300 on the screen, one ordinary field, a password field, a bank
/// field and an ALV grid with an IBAN column.
ScreenState logon_screen() {
    ScreenState s;
    s.complete = true;
    s.window_id = "wnd[0]";
    s.title = "Change Vendor: Bank Details";
    s.transaction = "XK02";
    s.window = Rect{100, 50, 400, 300};
    s.fields.push_back(field("wnd[0]", "GuiMainWindow", Rect{100, 50, 400, 300}));
    s.fields.push_back(field("wnd[0]/usr/txtEKPO-MENGE", "GuiTextField", Rect{110, 60, 60, 16}, "Quantity", "120"));
    s.fields.push_back(field("wnd[0]/usr/pwdRSYST-BCODE", "GuiPasswordField", Rect{150, 100, 80, 20}, "Password"));
    s.fields.push_back(field("wnd[0]/usr/txtLFBK-BANKN", "GuiTextField", Rect{150, 140, 120, 20}, "Bank Account", "0012345678"));
    Field grid = field("wnd[0]/usr/cntlGRID/shellcont/shell", "GuiShell", Rect{120, 200, 400, 200});  // reaches past the window
    grid.columns = {"LIFNR", "IBAN", "NAME1"};
    s.fields.push_back(grid);
    return s;
}

/// SAP GUI at the COM boundary: what the screen shows, what HardCopyToMemory returns and the field values; records
/// on which thread it was called.
struct FakeSource : ScreenSource {
    std::mutex mu;
    ScreenState screen = logon_screen();
    Image image = test_image(400, 300);
    std::map<std::string, std::string> values{{"wnd[0]/usr/txtEKPO-MENGE", "120"}, {"wnd[0]/usr/txtLFBK-BANKN", "0012345678"}};
    std::vector<std::thread::id> threads;
    int captures = 0;
    bool fail_capture = false;
    ScreenState state(const std::function<bool(const Field&)>& needs_rect) override {
        std::lock_guard<std::mutex> l(mu);
        threads.push_back(std::this_thread::get_id());
        ScreenState s = screen;
        for (auto& f : s.fields) {
            if (auto v = values.find(f.id); v != values.end()) f.value = v->second;
            if (!needs_rect || !needs_rect(f)) f.rect.reset();  // only what the recorder asked for
        }
        return s;
    }
    std::string capture_png(const std::string& window_id) override {
        std::lock_guard<std::mutex> l(mu);
        threads.push_back(std::this_thread::get_id());
        captures++;
        if (fail_capture || window_id != screen.window_id) return {};
        return encode_png(image);
    }
    std::optional<std::string> value(const std::string& id, std::optional<int>, const std::string&) override {
        std::lock_guard<std::mutex> l(mu);
        auto v = values.find(id);
        if (v == values.end()) return std::nullopt;
        return v->second;
    }
};

ffy::link::StepRequest step(const std::string& tool, bool shots, bool blur, json args = json::object()) {
    ffy::link::StepRequest s;
    s.request_id = "inst." + ffy::rand_hex(8);
    s.tool = tool;
    s.args = std::move(args);
    s.shots = shots;
    s.blur = blur;
    return s;
}

ffy::proto::TrailPolicy on_policy(const std::string& mode = "every", bool blur = true) {
    ffy::proto::TrailPolicy p;
    p.on = true;
    p.since = 1761264000;  // 2025-10-24
    p.mode = mode;
    p.retention_days = 90;
    p.blur = blur;
    return p;
}

/// A recorder whose "main thread" is the calling thread (the marshalling has its own test below).
std::shared_ptr<TrailRecorder> direct_recorder(std::shared_ptr<FakeSource> src, std::vector<ffy::proto::TrailPolicy>* seen = nullptr) {
    RecorderOptions o;
    o.source = src;
    o.run_on_main = [](std::function<void()> fn) {
        fn();
        return true;
    };
    o.on_policy = [seen](const ffy::proto::TrailPolicy& p) {
        if (seen) seen->push_back(p);
    };
    return std::make_shared<TrailRecorder>(o);
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

fs::path temp_dir(const char* what) {
    auto d = fs::temp_directory_path() / (std::string("ffy-trail-") + what + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(d);
    return d;
}

}  // namespace

// ---- the field list -----------------------------------------------------------------------------------------------

TEST_CASE("trail fields: password and bank fields are recognised by SAP name, control type and screen label", "[trail]") {
    // bank fields (libffl's list: the gateway masks the same ones)
    for (const char* id : {"wnd[0]/usr/txtLFBK-BANKN[2,0]", "wnd[0]/usr/ctxtLFBK-BANKL[1,0]", "wnd[0]/usr/ctxtLFBK-BANKS[0,0]",
                           "wnd[0]/usr/txtTIBAN-IBAN", "wnd[0]/usr/txtBNKA-SWIFT", "wnd[0]/usr/txtLFBK-BKREF", "wnd[0]/usr/txtLFBK-KOINH",
                           "wnd[0]/usr/txtBUT0BK-ACCNAME", "wnd[0]/usr/txtLFBK-BKONT", "wnd[0]/usr/txtCCARD-CCNUM"}) {
        INFO(id);
        auto f = field(id, "GuiTextField", Rect{0, 0, 10, 10});
        CHECK(is_bank(f));
        CHECK(must_blur(f, true));
        CHECK_FALSE(must_blur(f, false));  // bank fields only while blurring is on
        CHECK_FALSE(is_password(f));
    }
    // by the label on the screen when the SAP name says nothing (custom fields)
    CHECK(must_blur(field("wnd[0]/usr/txtZZ_FOO", "GuiTextField", {}, "IBAN"), true));
    CHECK(must_blur(field("wnd[0]/usr/txtZZ_BAR", "GuiTextField", {}, "Kontoinhaber"), true));
    CHECK(must_blur(field("wnd[0]/usr/txtZZ_BAZ", "GuiTextField", {}, "Account number"), true));
    // passwords: always, also with blurring off
    for (auto f : {field("wnd[0]/usr/pwdRSYST-BCODE", "GuiPasswordField", {}), field("wnd[1]/usr/txtXU400-NEWCODE", "GuiTextField", {}),
                   field("wnd[0]/usr/txtZZ_X", "GuiTextField", {}, "Kennwort"), field("wnd[0]/usr/txtZZ_Y", "GuiTextField", {}, "PIN"),
                   field("wnd[0]/usr/txtZZ_Z", "GuiPasswordField", {}, "Name")}) {
        INFO(f.id);
        CHECK(is_password(f));
        CHECK(must_blur(f, true));
        CHECK(must_blur(f, false));
    }
    // grid columns
    Field grid = field("wnd[0]/usr/cntlGRID/shellcont/shell", "GuiShell", {});
    grid.columns = {"LIFNR", "NAME1"};
    CHECK_FALSE(must_blur(grid, true));
    grid.columns.push_back("BANKN");
    CHECK(must_blur(grid, true));
    // ordinary fields stay as they are
    for (auto f : {field("wnd[0]/usr/txtEKPO-MENGE", "GuiTextField", {}, "Quantity"), field("wnd[0]/usr/ctxtEKKO-LIFNR", "GuiCTextField", {}, "Vendor"),
                   field("wnd[0]/usr/txtADRC-NAME1", "GuiTextField", {}, "Name"), field("wnd[0]/usr/ctxtVBAK-VSBED", "GuiCTextField", {}, "Shipping Conditions"),
                   field("wnd[0]/usr/btnSAVE", "GuiButton", {}, "", "Save"), field("wnd[0]/sbar", "GuiStatusbar", {})}) {
        INFO(f.id);
        CHECK_FALSE(must_blur(f, true));
    }
    // the window itself is never one rectangle to fill: its fields are
    CHECK_FALSE(must_blur(field("wnd[0]", "GuiMainWindow", {}, "Bank Account"), true));
    // content that could not be read (a control container, a grid without readable columns): always as a whole
    Field opaque = field("wnd[0]/usr/cntlCONTAINER", "GuiCustomControl", {});
    CHECK_FALSE(must_blur(opaque, true));
    opaque.opaque = true;
    CHECK(must_blur(opaque, true));
    CHECK(must_blur(opaque, false));
    // full scripting ids become the relative form the gateway shows and matches
    CHECK(relative_id("/app/con[0]/ses[0]/wnd[0]/usr/txtLFBK-BANKN") == "wnd[0]/usr/txtLFBK-BANKN");
    CHECK(relative_id("wnd[1]/usr/pwdX") == "wnd[1]/usr/pwdX");
}

// ---- blurring -------------------------------------------------------------------------------------------------------

TEST_CASE("trail blur: the pixels of password and bank fields are filled, everything else stays", "[trail]") {
    const ScreenState s = logon_screen();
    const Image original = test_image(400, 300);
    auto plan = plan_blur(s, 400, 300, true);
    REQUIRE_FALSE(plan.drop);
    CHECK(plan.blurred == 3);  // password, bank field, grid with IBAN column
    Image img = original;
    fill_rects(img, plan.rects);
    // password field at screen (150,100) 80x20 = image (50,50)..(130,70), padded by kBlurPadding
    for (int y = 50 - kBlurPadding; y < 70 + kBlurPadding; y++)
        for (int x = 50 - kBlurPadding; x < 130 + kBlurPadding; x++) REQUIRE(is_fill(img, x, y));
    // bank field (150,140) 120x20 and the grid (120,200) 300x100 (clamped to the image)
    CHECK(is_fill(img, 60, 100));
    CHECK(is_fill(img, 160, 95));
    CHECK(is_fill(img, 30, 160));
    CHECK(is_fill(img, 399, 299));
    // outside: unchanged (the quantity field at (110,60) and a free area)
    CHECK(same(img, original, 15, 15));
    CHECK(same(img, original, 50 - kBlurPadding - 1, 60));
    CHECK(same(img, original, 135, 60));
    CHECK(same(img, original, 5, 120));
    int changed_outside = 0;
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 400; x++) changed_outside += !same(img, original, x, y);
    CHECK(changed_outside == 0);

    // blurring off: passwords still, bank fields not
    auto off = plan_blur(s, 400, 300, false);
    REQUIRE_FALSE(off.drop);
    CHECK(off.blurred == 1);

    // a screenshot taken at twice the window's size (high DPI): the rectangles scale with it
    auto hi = plan_blur(s, 800, 600, true);
    REQUIRE_FALSE(hi.drop);
    Image big = test_image(800, 600);
    fill_rects(big, hi.rects);
    CHECK(is_fill(big, 100, 100));
    CHECK(is_fill(big, 259, 139));
    CHECK_FALSE(is_fill(big, 30, 30));

    // an opaque container is filled as a whole
    ScreenState hidden = s;
    hidden.fields = {field("wnd[0]/usr/cntlCC", "GuiCustomControl", Rect{120, 200, 100, 50})};
    hidden.fields[0].opaque = true;
    auto h = plan_blur(hidden, 400, 300, false);
    REQUIRE_FALSE(h.drop);
    CHECK(h.blurred == 1);

    // a field outside the captured window is not on the screenshot: nothing to fill, nothing counted
    ScreenState away = s;
    away.fields = {field("wnd[0]/usr/pwdX", "GuiPasswordField", Rect{2000, 2000, 50, 10})};
    auto a = plan_blur(away, 400, 300, true);
    CHECK_FALSE(a.drop);
    CHECK(a.blurred == 0);
    CHECK(a.rects.empty());
}

TEST_CASE("trail blur: a screenshot whose sensitive field cannot be located is dropped, never sent unblurred", "[trail]") {
    ScreenState s = logon_screen();
    for (auto& f : s.fields)
        if (f.type == "GuiPasswordField") f.rect.reset();
    auto plan = plan_blur(s, 400, 300, true);
    CHECK(plan.drop);
    CHECK(plan.rects.empty());
    CHECK(plan.blurred == 0);
    std::string why;
    CHECK_FALSE(make_shot(encode_png(test_image(400, 300)), s, true, &why));
    CHECK_FALSE(why.empty());
    CHECK(why.find("password") != std::string::npos);

    // a bank field without rectangle: dropped with blurring on, fine with blurring off (not blurred then)
    ScreenState b = logon_screen();
    for (auto& f : b.fields)
        if (f.id == "wnd[0]/usr/txtLFBK-BANKN") f.rect = Rect{0, 0, 0, 0};
    CHECK(plan_blur(b, 400, 300, true).drop);
    CHECK_FALSE(plan_blur(b, 400, 300, false).drop);

    // elements not readable at all, window position unknown, an image that does not match the window: dropped
    ScreenState incomplete = logon_screen();
    incomplete.complete = false;
    CHECK(plan_blur(incomplete, 400, 300, true).drop);
    CHECK(plan_blur(incomplete, 400, 300, false).drop);
    ScreenState nowin = logon_screen();
    nowin.window.reset();
    CHECK(plan_blur(nowin, 400, 300, true).drop);
    CHECK(plan_blur(logon_screen(), 400, 260, true).drop);  // e.g. only the client area was captured
    CHECK(plan_blur(logon_screen(), 400, 297, true).drop);  // 3 px off already (the padding is 3 px)
    CHECK_FALSE(plan_blur(logon_screen(), 400, 301, true).drop);  // rounding of a scaled capture
    CHECK(plan_blur(logon_screen(), 100, 75, true).drop);   // scale 0.25: not a display scaling

    // a screen without sensitive fields needs no positions
    ScreenState plain;
    plain.complete = true;
    plain.fields = {field("wnd[0]/usr/txtEKPO-MENGE", "GuiTextField", std::nullopt)};
    CHECK_FALSE(plan_blur(plain, 400, 300, true).drop);
}

TEST_CASE("trail shots: PNG in memory, filled before scaling, at most 1920 px wide and within the size limit", "[trail]") {
    // round trip
    const Image img = test_image(64, 48);
    const std::string png = encode_png(img);
    REQUIRE(png.size() > 8);
    CHECK(png.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0);
    auto back = decode_png(png);
    REQUIRE(back);
    CHECK(back->width == 64);
    CHECK(back->height == 48);
    CHECK(back->rgb == img.rgb);
    CHECK_FALSE(decode_png("not a png"));
    CHECK_FALSE(decode_png(std::string("\x89PNG\r\n\x1a\n", 8) + "garbage"));

    // the whole pipeline: the output is a PNG whose sensitive pixels are filled
    std::string why;
    auto shot = make_shot(encode_png(test_image(400, 300)), logon_screen(), true, &why);
    REQUIRE(shot);
    CHECK(shot->blurred == 3);
    auto out = decode_png(shot->png);
    REQUIRE(out);
    CHECK(out->width == 400);
    CHECK(is_fill(*out, 90, 60));
    CHECK_FALSE(is_fill(*out, 15, 15));

    // a wide screen is scaled to 1920 px; the fill survives scaling (it happens first)
    ScreenState wide = logon_screen();
    wide.window = Rect{0, 0, 3840, 2160};
    wide.fields = {field("wnd[0]/usr/pwdRSYST-BCODE", "GuiPasswordField", Rect{1000, 1000, 400, 100})};
    auto w = make_shot(encode_png(test_image(3840, 2160)), wide, true, &why);
    REQUIRE(w);
    auto wd = decode_png(w->png);
    REQUIRE(wd);
    CHECK(wd->width == 1920);
    CHECK(wd->height == 1080);
    CHECK(is_fill(*wd, 600, 525));

    // too large for the limit: scaled down until it fits
    ScreenState mid = wide;
    mid.window = Rect{0, 0, 1600, 1200};
    auto full = make_shot(encode_png(block_image(1600, 1200)), mid, true, &why);
    REQUIRE(full);
    const size_t limit = full->png.size() - 1;
    auto shrunk = make_shot(encode_png(block_image(1600, 1200)), mid, true, &why, limit);
    REQUIRE(shrunk);
    CHECK(shrunk->png.size() <= limit);
    CHECK(decode_png(shrunk->png)->width < 1600);
    CHECK(shrunk->blurred == 1);
    // not a PNG at all: nothing
    CHECK_FALSE(make_shot("garbage", logon_screen(), true, &why));
}

// ---- field changes --------------------------------------------------------------------------------------------------

TEST_CASE("trail changes: old and new values, passwords always masked, bank values while blurring is on", "[trail]") {
    auto f_qty = field("wnd[0]/usr/txtEKPO-MENGE", "GuiTextField", {}, "Quantity");
    auto f_pwd = field("wnd[0]/usr/pwdRSYST-BCODE", "GuiPasswordField", {}, "Password");
    auto f_bank = field("wnd[0]/usr/txtLFBK-BANKN", "GuiTextField", {}, "Bank Account");
    CHECK(masked_value(f_qty, "150", true) == "150");
    CHECK(masked_value(f_pwd, "secret", true) == ffy::proto::kMasked);
    CHECK(masked_value(f_pwd, "secret", false) == ffy::proto::kMasked);
    CHECK(masked_value(f_bank, "0012345678", true) == ffy::proto::kMasked);
    CHECK(masked_value(f_bank, "0012345678", false) == "0012345678");

    ScreenState before = logon_screen(), after = logon_screen();
    for (auto& f : after.fields) {
        if (f.id == "wnd[0]/usr/txtEKPO-MENGE") f.value = "150";
        if (f.id == "wnd[0]/usr/txtLFBK-BANKN") f.value = "0099999999";
    }
    auto changes = diff_fields(before, after, true);
    REQUIRE(changes.size() == 2);
    CHECK(changes[0].field == "wnd[0]/usr/txtEKPO-MENGE");
    CHECK(changes[0].label == "Quantity");
    CHECK(changes[0].old_value == "120");
    CHECK(changes[0].new_value == "150");
    CHECK(changes[1].old_value == ffy::proto::kMasked);
    CHECK(changes[1].new_value == ffy::proto::kMasked);
    auto open = diff_fields(before, after, false);
    REQUIRE(open.size() == 2);
    CHECK(open[1].new_value == "0099999999");
    // sensitive on either screen is enough: the label "IBAN" only appears after the change
    ScreenState b2 = logon_screen(), a2 = logon_screen();
    b2.fields.push_back(field("wnd[0]/usr/txtZZ_F", "GuiTextField", {}, "", "old"));
    a2.fields.push_back(field("wnd[0]/usr/txtZZ_F", "GuiTextField", {}, "", "new"));
    a2.fields.back().label = "IBAN";
    auto either = diff_fields(b2, a2, true);
    REQUIRE(either.size() == 1);
    CHECK(either[0].old_value == ffy::proto::kMasked);
    std::swap(b2.fields.back().label, a2.fields.back().label);
    CHECK(diff_fields(b2, a2, true)[0].new_value == ffy::proto::kMasked);

    // the recorder: a fill of the quantity, of the bank account, of a password field
    auto src = std::make_shared<FakeSource>();
    auto rec = direct_recorder(src);
    auto s = step("gui_element_fill", false, true, {{"element", "wnd[0]/usr/txtEKPO-MENGE"}, {"value", "150"}});
    rec->before(s);
    src->values["wnd[0]/usr/txtEKPO-MENGE"] = "150";  // what the tool call did
    auto r = rec->after(s, {{"isError", false}});
    REQUIRE(r.changes.size() == 1);
    CHECK(r.changes[0].field == "wnd[0]/usr/txtEKPO-MENGE");
    CHECK(r.changes[0].label == "Quantity");
    CHECK(r.changes[0].old_value == "120");
    CHECK(r.changes[0].new_value == "150");
    CHECK(r.description == "Changed Quantity on \"Change Vendor: Bank Details\"");
    CHECK(r.shots.empty());
    CHECK(r.description.find("150") == std::string::npos);  // never the typed value

    for (bool blur : {true, false}) {
        auto b = step("gui_element_fill", false, blur, {{"id", "wnd[0]/usr/txtLFBK-BANKN"}, {"value", "0099999999"}});
        rec->before(b);
        src->values["wnd[0]/usr/txtLFBK-BANKN"] = blur ? "0099999999" : "0011111111";
        auto br = rec->after(b, {{"isError", false}});
        REQUIRE(br.changes.size() == 1);
        CHECK(br.changes[0].field == "wnd[0]/usr/txtLFBK-BANKN");
        if (blur) {
            CHECK(br.changes[0].old_value == ffy::proto::kMasked);
            CHECK(br.changes[0].new_value == ffy::proto::kMasked);
        } else {
            CHECK(br.changes[0].old_value == "0099999999");
            CHECK(br.changes[0].new_value == "0011111111");
        }
        // libffl and the gateway match the same list on these strings
        CHECK(ffy::proto::bank_field(br.changes[0].field, br.changes[0].label));
    }

    auto p = step("gui_element_fill", false, false, {{"element", "wnd[0]/usr/pwdRSYST-BCODE"}, {"value", "hunter2"}});
    rec->before(p);
    auto pr = rec->after(p, {{"isError", false}});
    REQUIRE(pr.changes.size() == 1);
    CHECK(pr.changes[0].old_value == ffy::proto::kMasked);
    CHECK(pr.changes[0].new_value == ffy::proto::kMasked);
    CHECK(ffy::proto::password_field(pr.changes[0].field, pr.changes[0].label));

    // a fill target that is not on the screen is not read at all (its label is unknown): no change reported
    auto ghost = step("gui_element_fill", false, false, {{"element", "wnd[0]/usr/txtZZ_UNKNOWN"}, {"value", "1234"}});
    src->values["wnd[0]/usr/txtZZ_UNKNOWN"] = "4321";
    rec->before(ghost);
    src->values["wnd[0]/usr/txtZZ_UNKNOWN"] = "1234";
    CHECK(rec->after(ghost, {{"isError", false}}).changes.empty());
    // a full scripting id finds the field like the relative one
    auto full = step("gui_element_fill", false, true, {{"element", "/app/con[0]/ses[0]/wnd[0]/usr/txtEKPO-MENGE"}, {"value", "160"}});
    rec->before(full);
    src->values["wnd[0]/usr/txtEKPO-MENGE"] = "160";
    auto fr = rec->after(full, {{"isError", false}});
    REQUIRE(fr.changes.size() == 1);
    CHECK(fr.changes[0].field == "wnd[0]/usr/txtEKPO-MENGE");
    CHECK(fr.changes[0].new_value == "160");
    src->values["wnd[0]/usr/txtEKPO-MENGE"] = "150";

    // a read changes nothing; a tool marked destructive reports what changed on the screen
    auto read = step("gui_screen_read", false, true);
    rec->before(read);
    CHECK(rec->after(read, json::object()).changes.empty());
    CHECK_FALSE(rec->changes_sap("gui_key_send"));
    rec->set_change_tools({"gui_key_send"});
    CHECK(rec->changes_sap("gui_key_send"));
    CHECK(rec->changes_sap("gui_element_fill"));
    auto key = step("gui_key_send", false, true, {{"key", "11"}});
    rec->before(key);
    src->values["wnd[0]/usr/txtEKPO-MENGE"] = "175";
    auto kr = rec->after(key, {{"isError", false}});
    REQUIRE(kr.changes.size() == 1);
    CHECK(kr.changes[0].old_value == "150");
    CHECK(kr.changes[0].new_value == "175");
    CHECK(kr.description.rfind("Pressed 11", 0) == 0);
}

TEST_CASE("trail descriptions: one short line per step without values", "[trail]") {
    ScreenState s = logon_screen();
    CHECK(describe("gui_transaction_start", {{"code", "ME23N"}}, nullptr, &s, {}, false) == "Opened transaction ME23N \"Change Vendor: Bank Details\"");
    CHECK(describe("gui_screen_read", json::object(), &s, &s, {}, false) == "Read screen \"Change Vendor: Bank Details\"");
    CHECK(describe("gui_element_fill", {{"element", "wnd[0]/usr/txtEKPO-MENGE"}, {"value", "9"}}, &s, &s,
                   {{"wnd[0]/usr/txtEKPO-MENGE", "Quantity", "1", "9"}, {"wnd[0]/usr/txtEKET-EINDT", "Delivery date", "a", "b"}}, false) ==
          "Changed Quantity and Delivery date on \"Change Vendor: Bank Details\"");
    ScreenState with_button = s;
    with_button.fields.push_back(field("wnd[0]/tbar[0]/btn[11]", "GuiButton", {}, "", "Save"));
    CHECK(describe("gui_element_click", {{"element", "wnd[0]/tbar[0]/btn[11]"}}, &with_button, &s, {}, false) ==
          "Clicked Save on \"Change Vendor: Bank Details\"");
    // a clicked input field never shows its content
    CHECK(describe("gui_element_click", {{"element", "wnd[0]/usr/txtLFBK-BANKN"}}, &s, &s, {}, false).find("0012345678") == std::string::npos);
    CHECK(describe("gui_popup_close", json::object(), &s, nullptr, {}, true) == "Closed popup \"Change Vendor: Bank Details\" (failed)");
    CHECK(describe("gui_session_list", json::object(), nullptr, nullptr, {}, false) == "Session list");
}

// ---- the recorder ---------------------------------------------------------------------------------------------------

TEST_CASE("trail recorder: before and after screenshots with a fake SAP GUI; none when the step asks for none", "[trail]") {
    auto src = std::make_shared<FakeSource>();
    auto rec = direct_recorder(src);
    CHECK(rec->can_blur());
    auto s = step("gui_screen_read", true, true);
    rec->before(s);
    src->image = test_image(400, 300);
    auto r = rec->after(s, json::object());
    REQUIRE(r.shots.size() == 2);
    CHECK(r.shots[0].kind == "before");
    CHECK(r.shots[1].kind == "after");
    for (auto& sh : r.shots) {
        CHECK(sh.mime == "image/png");
        auto img = decode_png(sh.bytes);
        REQUIRE(img);
        CHECK(is_fill(*img, 90, 60));  // the password field
        CHECK(sh.bytes.size() <= ffy::proto::kShotMaxBytes);
    }
    CHECK(r.blurred == 6);  // 3 per screenshot
    CHECK(r.description == "Read screen \"Change Vendor: Bank Details\"");
    CHECK(src->captures == 2);

    // no screenshots asked: none taken
    auto n = step("gui_screen_read", false, true);
    rec->before(n);
    auto nr = rec->after(n, json::object());
    CHECK(nr.shots.empty());
    CHECK(nr.blurred == 0);
    CHECK(src->captures == 2);
    CHECK_FALSE(nr.description.empty());

    // capture errors never fail the step: it is recorded without the screenshot
    src->fail_capture = true;
    auto f = step("gui_screen_read", true, true);
    rec->before(f);
    auto fr = rec->after(f, json::object());
    CHECK(fr.shots.empty());
    CHECK_FALSE(fr.description.empty());
    src->fail_capture = false;

    // a popup opened between reading the elements and the capture: its image never gets the old window's rectangles
    struct PopupSource : FakeSource {
        std::string capture_png(const std::string& window_id) override { return FakeSource::capture_png(window_id == "wnd[0]" ? "wnd[1]" : window_id); }
    };
    auto popup = std::make_shared<PopupSource>();
    auto rec3 = direct_recorder(popup);
    auto ps = step("gui_screen_read", true, true);
    rec3->before(ps);
    CHECK(rec3->after(ps, json::object()).shots.empty());

    // a password field that cannot be located: no screenshot of that screen
    struct NoRect : FakeSource {
        ScreenState state(const std::function<bool(const Field&)>&) override {
            ScreenState s = FakeSource::state(nullptr);
            return s;
        }
    };
    auto blind = std::make_shared<NoRect>();
    auto rec2 = direct_recorder(blind);
    auto b = step("gui_screen_read", true, true);
    rec2->before(b);
    auto br = rec2->after(b, json::object());
    CHECK(br.shots.empty());
    CHECK_FALSE(br.description.empty());
}

TEST_CASE("trail recorder: SAP GUI is read on the main thread only, through MainThreadTools", "[trail][link]") {
    auto src = std::make_shared<FakeSource>();
    auto tools = std::make_shared<MainThreadTools>();
    RecorderOptions o;
    o.source = src;
    o.run_on_main = [tools](std::function<void()> fn) { return tools->invoke(std::move(fn), 5000); };
    auto rec = std::make_shared<TrailRecorder>(o);
    std::atomic<bool> stop{false};
    ffy::link::StepRecord r;
    std::thread worker([&] {  // libffl's agent calls the recorder on its worker threads
        auto s = step("gui_screen_read", true, true);
        rec->before(s);
        r = rec->after(s, json::object());
        stop = true;
    });
    tools->pump(stop);  // this thread plays fairyfly's main (COM) thread
    worker.join();
    CHECK(r.shots.size() == 2);
    REQUIRE_FALSE(src->threads.empty());
    for (auto id : src->threads) CHECK(id == std::this_thread::get_id());

    // nobody pumps (the link is stopping): the step is recorded without reading SAP, after the timeout
    RecorderOptions slow = o;
    slow.run_on_main = [tools](std::function<void()> fn) { return tools->invoke(std::move(fn), 50); };
    auto rec2 = std::make_shared<TrailRecorder>(slow);
    const size_t calls = src->threads.size();
    auto s = step("gui_screen_read", true, true);
    rec2->before(s);
    auto r2 = rec2->after(s, json::object());
    CHECK(r2.shots.empty());
    CHECK_FALSE(r2.description.empty());
    CHECK_FALSE(tools->pump_once(std::chrono::milliseconds(10)));  // the timed-out reads never run later
    CHECK(src->threads.size() == calls);
    tools->shutdown();
    CHECK_FALSE(tools->invoke([] {}, 1000));
}

// ---- the notice -----------------------------------------------------------------------------------------------------

TEST_CASE("trail notice: policy on and off reach the console line and the status file the tray reads", "[trail][link]") {
    std::vector<ffy::proto::TrailPolicy> seen;
    auto rec = direct_recorder(std::make_shared<FakeSource>(), &seen);
    rec->policy(on_policy());
    REQUIRE(seen.size() == 1);
    CHECK(rec->current_policy().on);
    CHECK(notice_line(seen[0]) ==
          "Audit trail ON: your administrator records the steps assistants take on this desktop, with screenshots (kept 90 days)");
    CHECK(notice_line(on_policy("changes", false)) ==
          "Audit trail ON: your administrator records the steps assistants take on this desktop, with screenshots of the steps "
          "that change SAP (kept 90 days); bank fields are not blurred");
    rec->policy(ffy::proto::TrailPolicy{});
    REQUIRE(seen.size() == 2);
    CHECK_FALSE(rec->current_policy().on);
    CHECK(notice_line(seen[1]).rfind("Audit trail off", 0) == 0);

    // `fairyfly link status`
    CHECK(status_text(on_policy().to_json()) == "on since 2025-10-24 (screenshots: every step; kept 90 days; blurring on)");
    CHECK(status_text(on_policy("changes", false).to_json()) == "on since 2025-10-24 (screenshots: only for changes; kept 90 days; blurring off)");
    CHECK(status_text(ffy::proto::TrailPolicy{}.to_json()) == "off");
    CHECK(status_text(json()) == "off");
    CHECK(status_text(json("on")) == "off");

    // the running link writes the policy into <state>.status.json; the tray and `link status` read it from there
    const auto dir = temp_dir("status");
    const std::string st = (dir / "state.json").string();
    write_status(st, "connected", json{{"trail", on_policy().to_json()}});
    auto status = read_status(st);
    REQUIRE(status.is_object());
    CHECK(status["state"] == "connected");
    CHECK(ffy::proto::TrailPolicy::from_json(status["trail"]).on);
    write_status(st, "connected");
    CHECK_FALSE(read_status(st).contains("trail"));

    // the supervisor rewrites the status as soon as the policy changes (not only on its heartbeat)
    std::atomic<bool> stop{false}, changed{false};
    json extra = {{"trail", ffy::proto::TrailPolicy{}.to_json()}};
    std::mutex m;
    SupervisorOptions so;
    so.poll = std::chrono::milliseconds(10);
    so.heartbeat = std::chrono::seconds(3600);
    so.status_changed = &changed;
    so.status_extra = [&] {
        std::lock_guard<std::mutex> l(m);
        return extra;
    };
    std::thread sup([&] { supervise(st, [](const std::atomic<bool>& net_stop) { while (!net_stop) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }, stop, so); });
    auto trail_on = [&] {
        auto s = read_status(st);
        return s.is_object() && s.contains("trail") && ffy::proto::TrailPolicy::from_json(s["trail"]).on;
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(trail_on());
    {
        std::lock_guard<std::mutex> l(m);
        extra = {{"trail", on_policy().to_json()}};
    }
    changed = true;
    bool seen_on = false;
    for (int i = 0; i < 300 && !seen_on; i++) {
        seen_on = trail_on();
        if (!seen_on) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(seen_on);
    stop = true;
    sup.join();
    fs::remove_all(dir);
}

// ---- with libffl's agent ------------------------------------------------------------------------------------------

namespace {
struct TestSigner : ffy::Signer {  // stands in for the gateway's per-tenant KMS key
    ffy::PKey k = ffy::ec_generate();
    std::string sign(const std::string& m) override { return ffy::es256_sign(k, m); }
    std::string kid() override { return ffy::jwk_from_ec(k).kid; }
    std::vector<ffy::Jwk> public_keys() override { return {ffy::jwk_from_ec(k)}; }
};

struct AgentRig {
    TestSigner gw;
    std::shared_ptr<FakeSource> src = std::make_shared<FakeSource>();
    std::shared_ptr<MainThreadTools> tools = std::make_shared<MainThreadTools>();
    std::shared_ptr<TrailRecorder> rec;
    std::unique_ptr<ffy::link::Agent> agent;
    std::vector<ffy::proto::TrailPolicy> notices;
    AgentRig() {
        tools->set_handlers(
            [] {
                return json::array({{{"name", "gui_screen_read"}, {"annotations", {{"readOnlyHint", true}}}},
                                    {{"name", "gui_element_fill"}, {"annotations", {{"destructiveHint", true}}}}});
            },
            [this](const std::string& name, const json& args, const ffy::link::CallInfo&) {
                if (name == "gui_element_fill") {
                    std::lock_guard<std::mutex> l(src->mu);
                    src->values[args.value("element", "")] = args.value("value", "");
                }
                return json{{"content", json::array({{{"type", "text"}, {"text", "ok"}}})}, {"isError", false}};
            });
        RecorderOptions o;
        o.source = src;
        o.run_on_main = [t = tools](std::function<void()> fn) { return t->invoke(std::move(fn), 5000); };
        o.on_policy = [this](const ffy::proto::TrailPolicy& p) { notices.push_back(p); };
        rec = std::make_shared<TrailRecorder>(o);
        ffy::link::State st;
        st.device_id = "dev_1"; st.issuer = "ffy-gateway"; st.tenant = "acme"; st.gateway_keys = gw.public_keys(); st.key = ffy::ec_generate();
        ffy::link::AgentConfig cfg;
        cfg.local = tools;
        cfg.recorder = rec;
        agent = std::make_unique<ffy::link::Agent>(st, cfg);
    }
    ffy::proto::Frame req(const std::string& tool, json args, json trail) {
        ffy::assertion::Claims c;
        c.iss = "ffy-gateway"; c.aud = "dev_1"; c.tenant = "acme"; c.sub = "lee@acme.example"; c.jti = ffy::rand_hex(8); c.client = "claude";
        c.iat = ffy::now_unix(); c.exp = c.iat + 60; c.req = "inst." + ffy::rand_hex(8);
        c.grants.tools = {"screen", "element"};
        c.grants.allow_write = true;
        c.grants.approval = "none";
        ffy::proto::Frame f;
        f.t = ffy::proto::TReq; f.id = c.req; f.assertion = ffy::assertion::sign(gw, c);
        f.rpc = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
        f.trail = trail;
        return f;
    }
    ffy::link::Agent::Outcome serve(const ffy::proto::Frame& f) {
        std::atomic<bool> stop{false};
        ffy::link::Agent::Outcome o;
        std::thread worker([&] {  // libffl serves each request on its own thread
            o = agent->serve(f);
            stop = true;
        });
        tools->pump(stop);
        worker.join();
        return o;
    }
};

std::string reassemble(const std::vector<ffy::proto::Frame>& frames, const std::string& kind) {
    std::string out;
    int next = 0;
    for (auto& f : frames)
        if (f.t == ffy::proto::TShot && f.kind == kind) {
            REQUIRE(f.seq == next++);
            out += ffy::b64std_decode(f.data);
        }
    return out;
}
}  // namespace

TEST_CASE("trail agent: with fairyfly's recorder a recorded call yields a step frame and blurred shot frames", "[trail][link]") {
    AgentRig rig;
    // the desktop announces the trail and blurring
    CHECK(rig.agent->caps() == std::vector<std::string>{"trail/1", "trail-blur/1"});
    const json per_call = {{"v", 1}, {"shots", true}, {"blur", true}};

    // without the policy: no recording, whatever the request says
    auto o = rig.serve(rig.req("gui_screen_read", json::object(), per_call));
    REQUIRE_FALSE(o.res.error);
    CHECK(o.trail.empty());
    CHECK(rig.src->captures == 0);

    // the policy arrives: the notice, then steps
    rig.agent->set_trail_policy(on_policy());
    REQUIRE(rig.notices.size() == 1);
    CHECK(rig.notices[0].on);
    o = rig.serve(rig.req("gui_screen_read", json::object(), per_call));
    REQUIRE_FALSE(o.res.error);
    REQUIRE_FALSE(o.trail.empty());
    CHECK(o.trail.front().t == ffy::proto::TStep);
    const json& step = o.trail.front().step;
    CHECK(step["description"] == "Read screen \"Change Vendor: Bank Details\"");
    CHECK(step["blurred"] == 6);
    CHECK(step["withheld"] == "");
    REQUIRE(step["shots"].size() == 2);
    for (const char* kind : {"before", "after"}) {
        INFO(kind);
        const std::string png = reassemble(o.trail, kind);
        auto img = decode_png(png);
        REQUIRE(img);
        CHECK(is_fill(*img, 90, 60));    // password field
        CHECK(is_fill(*img, 60, 100));   // bank account
        CHECK_FALSE(is_fill(*img, 15, 15));
    }
    CHECK(step["shots"][0]["sha256"] == ffy::hex_encode(ffy::sha256(reassemble(o.trail, "before"))));

    // a fill of the bank account with blurring: the step's change is masked, the assistant's answer has no trail data
    o = rig.serve(rig.req("gui_element_fill", {{"element", "wnd[0]/usr/txtLFBK-BANKN"}, {"value", "0099999999"}}, {{"v", 1}, {"shots", false}, {"blur", true}}));
    REQUIRE_FALSE(o.res.error);
    REQUIRE(o.trail.size() == 1);
    const json& fill = o.trail[0].step;
    REQUIRE(fill["changes"].size() == 1);
    CHECK(fill["changes"][0]["field"] == "wnd[0]/usr/txtLFBK-BANKN");
    CHECK(fill["changes"][0]["old"] == ffy::proto::kMasked);
    CHECK(fill["changes"][0]["new"] == ffy::proto::kMasked);
    CHECK(fill["shots"].empty());
    CHECK(o.res.rpc.dump().find("0099999999") == std::string::npos);
    for (auto& f : o.trail) CHECK(f.dump().find("0099999999") == std::string::npos);
    CHECK(o.trail[0].dump().find("0012345678") == std::string::npos);

    // a quantity: old -> new
    o = rig.serve(rig.req("gui_element_fill", {{"element", "wnd[0]/usr/txtEKPO-MENGE"}, {"value", "150"}}, {{"v", 1}, {"shots", false}, {"blur", true}}));
    REQUIRE(o.trail.size() == 1);
    CHECK(o.trail[0].step["changes"][0]["old"] == "120");
    CHECK(o.trail[0].step["changes"][0]["new"] == "150");

    // the call did not ask for a record: nothing
    o = rig.serve(rig.req("gui_screen_read", json::object(), nullptr));
    CHECK(o.trail.empty());

    // the admin turns it off: the notice goes, nothing is recorded
    rig.agent->set_trail_policy(ffy::proto::TrailPolicy{});
    REQUIRE(rig.notices.size() == 2);
    CHECK_FALSE(rig.notices[1].on);
    const int captures = rig.src->captures;
    o = rig.serve(rig.req("gui_screen_read", json::object(), per_call));
    CHECK(o.trail.empty());
    CHECK(rig.src->captures == captures);
}

// ---- state isolation ------------------------------------------------------------------------------------------------

TEST_CASE("link state guard: with FAIRYFLY_LINK_FORBID_DEFAULT_STATE the real pairing is never used", "[trail][link]") {
    const std::string dir = fs::path(default_state_path()).parent_path().string();
    CHECK(in_default_state_dir(default_state_path(), dir));
    CHECK(in_default_state_dir((fs::path(dir) / "state.json.paused").string(), dir));
    CHECK(in_default_state_dir((fs::path(dir) / "sub" / ".." / "state.json").string(), dir));
    CHECK(in_default_state_dir("", dir));  // no --state: the default
    CHECK_FALSE(in_default_state_dir((fs::temp_directory_path() / "ffy-test" / "state.json").string(), dir));
    CHECK_FALSE(in_default_state_dir(dir + "-trail-test/state.json", dir));  // a sibling directory is fine
#ifdef _WIN32
    std::string upper = default_state_path();
    for (auto& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    CHECK(in_default_state_dir(upper, dir));
#endif

    // The default location is redirected into a temporary profile first (LOCALAPPDATA / XDG_CONFIG_HOME): even a
    // broken guard could only touch that, never the pairing of the person at this desktop.
    const auto tmp = temp_dir("guard");
#ifdef _WIN32
    const char* profile_var = "LOCALAPPDATA";
#else
    const char* profile_var = "XDG_CONFIG_HOME";
#endif
    struct EnvRestore {  // also when a REQUIRE below fails
        const char* name;
        std::optional<std::string> old;
        explicit EnvRestore(const char* n) : name(n) {
            if (const char* v = std::getenv(n)) old = v;
        }
        ~EnvRestore() { set_env(name, old ? old->c_str() : nullptr); }
    };
    EnvRestore restore_profile(profile_var), restore_guard("FAIRYFLY_LINK_FORBID_DEFAULT_STATE");
    set_env(profile_var, (tmp / "profile").string().c_str());
    REQUIRE(default_state_path().rfind((tmp / "profile").string(), 0) == 0);
    const std::string fake_default = default_state_path();
    {
        ffy::link::State s;
        s.device_id = "dev_default"; s.issuer = "gw"; s.tenant = "acme"; s.key = ffy::ec_generate();
        ffy::link::save_state(s, fake_default);
    }
    set_env("FAIRYFLY_LINK_FORBID_DEFAULT_STATE", "1");
    LinkOptions real;  // no --state: the (redirected) default
    LinkOptions explicit_default;
    explicit_default.state_path = fake_default;
    for (const auto& o : {real, explicit_default}) {
        for (auto r : {link_status(o), link_pause(o), link_resume(o), link_unpair(o)}) {
            CHECK(r.status == fairyfly::Result::Status::Error);
            CHECK(r.error["code"] == "STATE_GUARD");
        }
        CHECK(run_link(o, nullptr, {}, nullptr, nullptr) == 2);
    }
    CHECK(fs::exists(fake_default));                       // not unpaired
    CHECK_FALSE(fs::exists(pause_marker(fake_default)));  // not paused

    // an isolated state works, and `link status` shows the audit trail of a running link
    LinkOptions own;
    own.state_path = (tmp / "state.json").string();
    auto st = link_status(own);
    CHECK(st.status == fairyfly::Result::Status::Success);
    CHECK(st.data["state"] == own.state_path);
    CHECK(st.data["paired"] == false);
    {
        ffy::link::State s;
        s.device_id = "dev_t"; s.issuer = "gw"; s.tenant = "acme"; s.tenant_name = "ACME"; s.email = "lee@acme.example";
        s.link_url = "wss://gw.example/link"; s.key = ffy::ec_generate();
        ffy::link::save_state(s, own.state_path);
    }
    write_status(own.state_path, "connected", json{{"trail", on_policy().to_json()}});
    st = link_status(own);
    CHECK(st.data["paired"] == true);
    CHECK(st.data["audit_trail"] == "on since 2025-10-24 (screenshots: every step; kept 90 days; blurring on)");
    auto view = make_link_control(own.state_path)->view();
    CHECK(view.trail_on);
    CHECK(view.trail_mode == "every");
    write_status(own.state_path, "stopped", json{{"trail", on_policy().to_json()}});
    CHECK(link_status(own).data["audit_trail"] == "off");  // a link that is not running records nothing
    CHECK_FALSE(make_link_control(own.state_path)->view().trail_on);
    REQUIRE(link_pause(own).status == fairyfly::Result::Status::Success);
    CHECK(is_paused(own.state_path));
    // the tray's view of the default pairing cannot pause it either
    CHECK_FALSE(make_link_control(fake_default)->set_paused(true));
    CHECK_FALSE(fs::exists(pause_marker(fake_default)));
    CHECK(link_state_guard(own) == std::nullopt);
    CHECK(link_state_guard(explicit_default).has_value());
    fs::remove_all(tmp);
}

// ---- live check (SAP GUI with a logged-in session; tests/integration/link_trail_live.ps1 runs it) -------------------

#ifdef _WIN32
TEST_CASE("trail live: the active SAP GUI window, blurred as the audit trail would send it", "[.][trail-live]") {
    const char* out_dir = std::getenv("FAIRYFLY_TRAIL_CHECK_DIR");
    if (!out_dir || !*out_dir) SKIP("FAIRYFLY_TRAIL_CHECK_DIR not set");
    fairyfly::sap::ComGuiApplicationPtr app;
    try {
        app = fairyfly::sap::ComGuiApplication::create();
    } catch (const std::exception& e) {
        SKIP(std::string("SAP GUI not available: ") + e.what());
    }
    if (!app || app->get_connection_count() == 0) SKIP("no SAP GUI connection");
    auto session = app->get_connection(0)->get_session(0);
    REQUIRE(session);
    auto source = make_com_screen_source_for([session] { return session; });
    const char* blur_env = std::getenv("FAIRYFLY_TRAIL_CHECK_BLUR");
    const bool blur = !blur_env || std::string(blur_env) != "0";
    ScreenState state = source->state([blur](const Field& f) { return must_blur(f, blur); });
    REQUIRE(state.complete);
    std::string raw = source->capture_png(state.window_id);
    REQUIRE_FALSE(raw.empty());
    auto native = decode_png(raw);
    REQUIRE(native);
    std::string why;
    auto shot = make_shot(raw, state, blur, &why);
    raw.clear();
    // what was found, without values: the script compares it with the picture
    json report = {{"window", state.window_id}, {"title", state.title}, {"transaction", state.transaction},
                   {"image", {{"width", native->width}, {"height", native->height}}}, {"blur", blur},
                   {"sent", bool(shot)}, {"dropped_because", shot ? "" : why}, {"blurred", shot ? shot->blurred : 0}, {"fields", json::array()}};
    if (state.window) report["window_rect"] = {state.window->x, state.window->y, state.window->width, state.window->height};
    for (const auto& f : state.fields) {
        if (!must_blur(f, blur)) continue;
        json j = {{"id", f.id}, {"type", f.type}, {"password", is_password(f)}, {"bank", is_bank(f)}};
        if (f.rect) j["rect"] = {f.rect->x, f.rect->y, f.rect->width, f.rect->height};
        report["fields"].push_back(j);
    }
    fs::create_directories(out_dir);
    std::ofstream(fs::path(out_dir) / "trail-check.json") << report.dump(2);
    if (shot) std::ofstream(fs::path(out_dir) / "trail-check.png", std::ios::binary) << shot->png;
    INFO(report.dump());
    CHECK((shot || !why.empty()));
}
#endif
