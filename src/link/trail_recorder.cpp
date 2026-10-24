#include "include/link/trail_recorder.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <exception>
#include <utility>

#include <png.h>
#include <spdlog/spdlog.h>

namespace fairyfly::link::trail {

using json = nlohmann::json;
namespace proto = ffy::proto;

// ---- the field list -------------------------------------------------------------------------------------------------

namespace {
bool window_type(const std::string& type) {
    return type == "GuiMainWindow" || type == "GuiModalWindow" || type == "GuiFrameWindow";
}
}  // namespace

bool is_password(const Field& f) {
    if (f.type == "GuiPasswordField") return true;
    if (proto::password_field(f.id, f.label) || proto::password_field(f.name, f.tooltip)) return true;
    for (const auto& c : f.columns)
        if (proto::password_field(c, "")) return true;
    return false;
}

bool is_bank(const Field& f) {
    if (proto::bank_field(f.id, f.label) || proto::bank_field(f.name, f.tooltip)) return true;
    for (const auto& c : f.columns)
        if (proto::bank_field(c, "")) return true;
    return false;
}

bool must_blur(const Field& f, bool blur) {
    if (window_type(f.type)) return false;  // the window itself: its children decide
    return is_password(f) || (blur && is_bank(f));
}

// ---- blurring -------------------------------------------------------------------------------------------------------

BlurPlan plan_blur(const ScreenState& screen, int image_width, int image_height, bool blur) {
    BlurPlan plan;
    auto drop = [&](const char* why) {
        plan.drop = true;
        plan.reason = why;
        plan.rects.clear();
        plan.blurred = 0;
        return plan;
    };
    if (!screen.complete) return drop("the screen's elements could not be read");
    if (image_width <= 0 || image_height <= 0) return drop("empty image");
    std::vector<const Field*> sensitive;
    for (const auto& f : screen.fields)
        if (must_blur(f, blur)) sensitive.push_back(&f);
    if (sensitive.empty()) return plan;
    if (!screen.window || screen.window->empty()) return drop("the window's position is unknown");
    const Rect& w = *screen.window;
    const double sx = double(image_width) / w.width, sy = double(image_height) / w.height;
    if (std::abs(sx - sy) / std::max(sx, sy) > kAspectTolerance) return drop("the image does not match the window's size");
    for (const Field* f : sensitive) {
        if (!f->rect || f->rect->empty()) return drop("a password or bank field's position is unknown");
        const Rect& r = *f->rect;
        int x0 = int(std::floor((r.x - w.x) * sx)) - kBlurPadding;
        int y0 = int(std::floor((r.y - w.y) * sy)) - kBlurPadding;
        int x1 = int(std::ceil((r.x + r.width - w.x) * sx)) + kBlurPadding;
        int y1 = int(std::ceil((r.y + r.height - w.y) * sy)) + kBlurPadding;
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, image_width);
        y1 = std::min(y1, image_height);
        if (x1 <= x0 || y1 <= y0) continue;  // outside the captured window: not on the screenshot
        plan.rects.push_back({x0, y0, x1 - x0, y1 - y0});
        plan.blurred++;
    }
    return plan;
}

void fill_rects(Image& image, const std::vector<Rect>& rects) {
    if (!image.valid()) return;
    for (const auto& r : rects) {
        const int x0 = std::clamp(r.x, 0, image.width), x1 = std::clamp(r.x + r.width, 0, image.width);
        const int y0 = std::clamp(r.y, 0, image.height), y1 = std::clamp(r.y + r.height, 0, image.height);
        for (int y = y0; y < y1; y++) {
            std::uint8_t* p = image.rgb.data() + (size_t(y) * size_t(image.width) + size_t(x0)) * 3;
            for (int x = x0; x < x1; x++, p += 3) {
                p[0] = 0x60;
                p[1] = 0x60;
                p[2] = 0x60;
            }
        }
    }
}

Image scale_to_width(const Image& in, int width) {
    if (!in.valid() || width <= 0 || width >= in.width) return in;
    Image out;
    out.width = width;
    out.height = std::max(1, int(std::lround(double(in.height) * width / in.width)));
    out.rgb.resize(size_t(out.width) * size_t(out.height) * 3);
    for (int y = 0; y < out.height; y++) {
        const int sy0 = int(int64_t(y) * in.height / out.height);
        const int sy1 = std::max(sy0 + 1, int(int64_t(y + 1) * in.height / out.height));
        for (int x = 0; x < out.width; x++) {
            const int sx0 = int(int64_t(x) * in.width / out.width);
            const int sx1 = std::max(sx0 + 1, int(int64_t(x + 1) * in.width / out.width));
            unsigned long sum[3] = {0, 0, 0};
            for (int yy = sy0; yy < sy1; yy++) {
                const std::uint8_t* p = in.rgb.data() + (size_t(yy) * size_t(in.width) + size_t(sx0)) * 3;
                for (int xx = sx0; xx < sx1; xx++, p += 3) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            const unsigned long n = (unsigned long)(sy1 - sy0) * (unsigned long)(sx1 - sx0);
            std::uint8_t* q = out.rgb.data() + (size_t(y) * size_t(out.width) + size_t(x)) * 3;
            for (int c = 0; c < 3; c++) q[c] = std::uint8_t((sum[c] + n / 2) / n);
        }
    }
    return out;
}

// ---- PNG ------------------------------------------------------------------------------------------------------------

std::optional<Image> decode_png(const std::string& bytes) {
    if (bytes.size() < 8 || bytes.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) != 0) return std::nullopt;
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, bytes.data(), bytes.size())) {
        png_image_free(&img);
        return std::nullopt;
    }
    if (img.width == 0 || img.height == 0 || img.width > 16384 || img.height > 16384) {
        png_image_free(&img);
        return std::nullopt;
    }
    img.format = PNG_FORMAT_RGB;
    Image out;
    out.width = int(img.width);
    out.height = int(img.height);
    out.rgb.resize(PNG_IMAGE_SIZE(img));
    if (!png_image_finish_read(&img, nullptr, out.rgb.data(), 0, nullptr)) {
        png_image_free(&img);
        return std::nullopt;
    }
    png_image_free(&img);
    if (!out.valid()) return std::nullopt;
    return out;
}

std::string encode_png(const Image& image) {
    if (!image.valid()) return {};
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    img.width = png_uint_32(image.width);
    img.height = png_uint_32(image.height);
    img.format = PNG_FORMAT_RGB;
    png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&img, nullptr, &size, 0, image.rgb.data(), 0, nullptr) || size == 0) {
        png_image_free(&img);
        return {};
    }
    std::string out(size, '\0');
    if (!png_image_write_to_memory(&img, out.data(), &size, 0, image.rgb.data(), 0, nullptr)) {
        png_image_free(&img);
        return {};
    }
    png_image_free(&img);
    out.resize(size);
    return out;
}

std::optional<Shot> make_shot(const std::string& raw_png, const ScreenState& screen, bool blur, std::string* why, size_t max_bytes) {
    auto fail = [&](const std::string& reason) -> std::optional<Shot> {
        if (why) *why = reason;
        return std::nullopt;
    };
    if (raw_png.empty()) return fail("no screenshot");
    auto img = decode_png(raw_png);
    if (!img) return fail("the screenshot is not a readable PNG");
    const BlurPlan plan = plan_blur(screen, img->width, img->height, blur);
    if (plan.drop) return fail(plan.reason);
    fill_rects(*img, plan.rects);  // before scaling: nothing of a sensitive field survives into the smaller image
    int width = std::min(img->width, kMaxShotWidth);
    for (int attempt = 0; attempt < 6; attempt++) {
        std::string png = encode_png(scale_to_width(*img, width));
        if (png.empty()) return fail("PNG encoding failed");
        if (png.size() <= max_bytes) return Shot{std::move(png), plan.blurred};
        width = width * 3 / 4;
        if (width < 320) break;
    }
    return fail("the screenshot is too large");
}

// ---- field changes and descriptions ---------------------------------------------------------------------------------

std::string masked_value(const Field& f, const std::string& value, bool blur) {
    if (is_password(f) || (blur && is_bank(f))) return proto::kMasked;
    return value;
}

std::vector<ffy::link::FieldChange> diff_fields(const ScreenState& before, const ScreenState& after, bool blur) {
    std::vector<ffy::link::FieldChange> out;
    if (!before.complete || !after.complete) return out;
    std::map<std::string, const Field*> old;
    for (const auto& f : before.fields)
        if (f.input) old.emplace(f.id, &f);
    for (const auto& f : after.fields) {
        if (!f.input) continue;
        auto i = old.find(f.id);
        if (i == old.end() || i->second->value == f.value) continue;
        if (is_password(f) || is_password(*i->second)) continue;  // its Text is never read: no change to report
        if (out.size() >= proto::kStepMaxChanges) break;
        out.push_back({f.id, f.label.empty() ? i->second->label : f.label, masked_value(f, i->second->value, blur), masked_value(f, f.value, blur)});
    }
    return out;
}

namespace {
std::string leaf(const std::string& id) {
    auto slash = id.rfind('/');
    return slash == std::string::npos ? id : id.substr(slash + 1);
}
const Field* find_field(const ScreenState* s, const std::string& id) {
    if (!s || id.empty()) return nullptr;
    for (const auto& f : s->fields)
        if (f.id == id) return &f;
    // "@active/usr/..." and ids the caller wrote without the window
    for (const auto& f : s->fields)
        if (f.id.size() > id.size() && f.id.compare(f.id.size() - id.size(), id.size(), id) == 0) return &f;
    if (id.rfind("@active", 0) == 0) {
        const std::string rest = id.substr(7);
        for (const auto& f : s->fields)
            if (f.id.size() >= rest.size() && f.id.compare(f.id.size() - rest.size(), rest.size(), rest) == 0) return &f;
    }
    return nullptr;
}
std::string quoted(const std::string& s) { return "\"" + s + "\""; }
std::string name_of(const ffy::link::FieldChange& c) { return c.label.empty() ? leaf(c.field) : c.label; }
std::string list_names(const std::vector<ffy::link::FieldChange>& changes) {
    std::string out;
    const size_t shown = std::min<size_t>(changes.size(), 3);
    for (size_t i = 0; i < shown; i++) {
        if (i > 0) out += i + 1 == shown && changes.size() == shown ? " and " : ", ";
        out += name_of(changes[i]);
    }
    if (changes.size() > shown) out += " and " + std::to_string(changes.size() - shown) + " more";
    return out;
}
std::string str_arg(const json& args, const char* key) {
    return args.is_object() && args.contains(key) && args[key].is_string() ? args[key].get<std::string>() : std::string();
}
/// What a button or menu entry says; never the content of an input field.
std::string element_name(const ScreenState* s, const std::string& id) {
    const Field* f = find_field(s, id);
    if (!f) return leaf(id);
    if (!f->label.empty()) return f->label;
    const bool caption = f->type == "GuiButton" || f->type == "GuiTab" || f->type == "GuiMenu" || f->type == "GuiCheckBox" ||
                         f->type == "GuiRadioButton";
    if (caption && !f->value.empty() && !must_blur(*f, true)) return f->value;
    if (!f->tooltip.empty()) return f->tooltip;
    return leaf(id);
}
}  // namespace

std::string describe(const std::string& tool, const json& args, const ScreenState* before, const ScreenState* after,
                     const std::vector<ffy::link::FieldChange>& changes, bool failed) {
    const ScreenState* now = after && !after->title.empty() ? after : before;
    const std::string title = now ? now->title : std::string();
    const std::string on = title.empty() ? std::string() : " on " + quoted(title);
    std::string d;
    if (tool == "gui_transaction_start") {
        const std::string code = str_arg(args, "code");
        d = "Opened transaction " + code + (title.empty() ? std::string() : " " + quoted(title));
    } else if (tool == "gui_element_fill") {
        const auto target = fill_target(args);
        if (!changes.empty()) d = "Changed " + list_names(changes) + on;
        else d = "Filled " + (target ? element_name(before, target->element) : std::string("a field")) + on;
    } else if (tool == "gui_element_click") {
        d = "Clicked " + element_name(before, str_arg(args, "element").empty() ? str_arg(args, "id") : str_arg(args, "element")) + on;
    } else if (tool == "gui_key_send") {
        d = "Pressed " + (str_arg(args, "key").empty() ? std::string("a key") : str_arg(args, "key")) + on;
    } else if (tool == "gui_menu_select") {
        d = "Selected menu " + str_arg(args, "path") + on;
    } else if (tool == "gui_popup_close") {
        d = "Closed popup" + (before && !before->title.empty() ? " " + quoted(before->title) : std::string());
    } else if (tool == "gui_screen_read") {
        d = "Read screen" + (title.empty() ? std::string() : " " + quoted(title));
    } else if (tool == "gui_screen_capture") {
        d = "Took a screenshot" + on;
    } else {
        std::string what = tool.rfind("gui_", 0) == 0 ? tool.substr(4) : tool;
        std::replace(what.begin(), what.end(), '_', ' ');
        if (!what.empty()) what[0] = char(std::toupper(static_cast<unsigned char>(what[0])));
        d = what + on;
        if (!changes.empty()) d += ", changed " + list_names(changes);
    }
    if (failed) d += " (failed)";
    return d;
}

// ---- the notice -----------------------------------------------------------------------------------------------------

std::string notice_line(const proto::TrailPolicy& p) {
    if (!p.on) return "Audit trail off: the steps assistants take on this desktop are no longer recorded";
    std::string s = "Audit trail ON: your administrator records the steps assistants take on this desktop, ";
    s += p.mode == "changes" ? "with screenshots of the steps that change SAP" : "with screenshots";
    s += " (kept " + std::to_string(p.retention_days) + " days)";
    if (!p.blur) s += "; bank fields are not blurred";
    return s;
}

std::string status_text(const json& trail) {
    const auto p = proto::TrailPolicy::from_json(trail);
    if (!p.on) return "off";
    std::string s = "on";
    if (p.since > 0) {
        std::time_t t = std::time_t(p.since);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[16];
        std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
        s += std::string(" since ") + buf;
    }
    s += std::string(" (screenshots: ") + (p.mode == "changes" ? "only for changes" : "every step") + "; kept " + std::to_string(p.retention_days) +
         " days; blurring " + (p.blur ? "on" : "off") + ")";
    return s;
}

// ---- the recorder ---------------------------------------------------------------------------------------------------

std::optional<FillTarget> fill_target(const json& args) {
    if (!args.is_object()) return std::nullopt;
    FillTarget t;
    for (auto k : {"element", "id", "element_id"})
        if (args.contains(k) && args[k].is_string() && !args[k].get<std::string>().empty()) {
            t.element = args[k].get<std::string>();
            break;
        }
    if (t.element.empty()) return std::nullopt;
    if (args.contains("row") && args["row"].is_number_integer()) t.row = args["row"].get<int>();
    if (args.contains("column") && args["column"].is_string()) t.column = args["column"].get<std::string>();
    return t;
}

TrailRecorder::TrailRecorder(RecorderOptions options) : o_(std::move(options)) {}

void TrailRecorder::policy(const proto::TrailPolicy& p) {
    {
        std::lock_guard<std::mutex> l(mu_);
        policy_ = p;
        if (!p.on) pending_.clear();
    }
    if (o_.on_policy) o_.on_policy(p);
}

proto::TrailPolicy TrailRecorder::current_policy() const {
    std::lock_guard<std::mutex> l(mu_);
    return policy_;
}

void TrailRecorder::set_change_tools(std::set<std::string> tools) {
    std::lock_guard<std::mutex> l(mu_);
    change_tools_ = std::move(tools);
}

bool TrailRecorder::changes_sap(const std::string& tool) const {
    if (tool == "gui_element_fill") return true;
    std::lock_guard<std::mutex> l(mu_);
    return change_tools_.count(tool) > 0;
}

std::optional<TrailRecorder::Snapshot> TrailRecorder::snapshot(const ffy::link::StepRequest& s, bool shot) {
    if (!o_.source || !o_.run_on_main) return std::nullopt;
    auto out = std::make_shared<Snapshot>();
    auto source = o_.source;
    const bool blur = s.blur;
    std::optional<FillTarget> target;
    if (s.tool == "gui_element_fill") target = fill_target(s.args);
    const bool ran = o_.run_on_main([out, source, blur, shot, target] {
        try {
            out->state = source->state([shot, blur](const Field& f) { return shot && must_blur(f, blur); });
            if (shot) out->raw_png = source->capture_png();
            if (target) {
                std::string id = target->element;
                if (id.rfind("@active", 0) == 0 && !out->state.window_id.empty()) id = out->state.window_id + id.substr(7);
                const Field* f = find_field(&out->state, id);
                if (f && is_password(*f)) out->target_value = std::string();  // never read a password
                else out->target_value = source->value(id, target->row, target->column);
            }
        } catch (const std::exception& e) {
            spdlog::warn("audit trail: reading the screen failed: {}", e.what());
        } catch (...) {
            spdlog::warn("audit trail: reading the screen failed");
        }
    });
    if (!ran) {
        spdlog::warn("audit trail: the screen could not be read in time");
        return std::nullopt;
    }
    return std::move(*out);
}

void TrailRecorder::before(const ffy::link::StepRequest& s) {
    try {
        if (!s.shots && !changes_sap(s.tool)) return;  // nothing to compare against: after() describes it
        auto snap = snapshot(s, s.shots);
        if (!snap) return;
        Pending p;
        p.state = std::move(snap->state);
        p.target_value = std::move(snap->target_value);
        if (s.shots) {
            std::string why;
            p.shot = make_shot(snap->raw_png, p.state, s.blur, &why);
            if (!p.shot) spdlog::info("audit trail: no before screenshot for {}: {}", s.tool, why);
        }
        snap->raw_png.clear();
        std::lock_guard<std::mutex> l(mu_);
        if (pending_.size() >= 16) pending_.clear();  // steps whose after() never came
        pending_[s.request_id] = std::move(p);
    } catch (const std::exception& e) {
        spdlog::warn("audit trail: before {} failed: {}", s.tool, e.what());
    }
}

ffy::link::StepRecord TrailRecorder::after(const ffy::link::StepRequest& s, const json& result) {
    ffy::link::StepRecord rec;
    const bool failed = result.is_object() && result.contains("isError") && result["isError"].is_boolean() && result["isError"].get<bool>();
    std::optional<Pending> before;
    {
        std::lock_guard<std::mutex> l(mu_);
        if (auto i = pending_.find(s.request_id); i != pending_.end()) {
            before = std::move(i->second);
            pending_.erase(i);
        }
    }
    std::optional<Snapshot> now;
    try {
        now = snapshot(s, s.shots);
        if (s.shots && before && before->shot) {
            rec.shots.push_back({"before", "image/png", std::move(before->shot->png)});
            rec.blurred += before->shot->blurred;
        }
        if (s.shots && now) {
            std::string why;
            auto shot = make_shot(now->raw_png, now->state, s.blur, &why);
            now->raw_png.clear();
            if (shot) {
                rec.shots.push_back({"after", "image/png", std::move(shot->png)});
                rec.blurred += shot->blurred;
            } else {
                spdlog::info("audit trail: no after screenshot for {}: {}", s.tool, why);
            }
        }
        if (changes_sap(s.tool) && before && now) {
            if (s.tool == "gui_element_fill") {
                if (auto t = fill_target(s.args); t && before->target_value && now->target_value) {
                    std::string id = t->element;
                    if (id.rfind("@active", 0) == 0 && !now->state.window_id.empty()) id = now->state.window_id + id.substr(7);
                    const Field* f = find_field(&now->state, id);
                    if (!f) f = find_field(&before->state, id);
                    Field target = f ? *f : Field{};
                    if (!f) target.id = id;
                    std::string field = f ? f->id : id;
                    std::string label = target.label;
                    if (t->row) {  // a grid cell: the column names the field
                        field += "[" + std::to_string(*t->row) + "," + t->column + "]";
                        target.id = field;
                        target.columns = {t->column};
                        if (label.empty()) label = t->column;
                    }
                    if (*before->target_value != *now->target_value || is_password(target))
                        rec.changes.push_back({field, label, masked_value(target, *before->target_value, s.blur),
                                               masked_value(target, *now->target_value, s.blur)});
                }
            }
            for (auto& c : diff_fields(before->state, now->state, s.blur)) {
                if (rec.changes.size() >= proto::kStepMaxChanges) break;
                bool dup = false;
                for (const auto& x : rec.changes) dup = dup || x.field == c.field;
                if (!dup) rec.changes.push_back(std::move(c));
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("audit trail: after {} failed: {}", s.tool, e.what());
    }
    try {
        rec.description = describe(s.tool, s.args, before ? &before->state : nullptr, now ? &now->state : nullptr, rec.changes, failed);
    } catch (const std::exception&) {
        rec.description = s.tool;
    }
    return rec;
}

}  // namespace fairyfly::link::trail
