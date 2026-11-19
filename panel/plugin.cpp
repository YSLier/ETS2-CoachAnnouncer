#include "panel_snapshot.hpp"

#include <SPF_KeyBinds_API.h>
#include <SPF_Localization_API.h>
#include <SPF_Manifest_API.h>
#include <SPF_Plugin.h>
#include <SPF_UI_API.h>

#include <algorithm>
#include <array>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <string_view>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using namespace coach::panel;
using Clock = std::chrono::steady_clock;

constexpr char plugin_name[] = "CoachAnnouncerPanel";
constexpr char window_id[] = "QuickSettings";
constexpr char action_id[] = "Panel.toggle";

enum class Connection { missing, old_core, inactive, busy, live };

struct Panel {
    const SPF_Core_API* core{};
    SPF_UI_API* ui{};
    SPF_Window_Handle* window{};
    SPF_KeyBinds_Handle* keys{};
    SPF_Localization_Handle* localization{};
    Snapshot view;
    Connection connection{};
    Clock::time_point next_poll{};
    Clock::time_point sent_at{};
    std::uint64_t pending{};
    Result result{Result::accepted};
    bool show_result{};
    bool reset_confirm{};
    bool zh{};
    int volume{};
    bool dragging_volume{};
    int approach{};
    bool editing_approach{};
    int binding{-1};
    char chord[40]{};

    const char* label(const char* english, const char* chinese) const { return zh ? chinese : english; }
};

Panel panel;
std::atomic<unsigned> toggle_requests{};

class CoreModule {
public:
#ifdef _WIN32
    CoreModule() { GetModuleHandleExW(0, L"CoachAnnouncer.dll", &module_); }
    ~CoreModule() { if (module_) FreeLibrary(module_); }
#else
    CoreModule() : module_(dlopen("CoachAnnouncer.so", RTLD_NOW | RTLD_NOLOAD)) {}
    ~CoreModule() { if (module_) dlclose(module_); }
#endif
    explicit operator bool() const { return module_ != nullptr; }
    template<class Proc>
    Proc get(const char* name) const {
        if (!module_) return nullptr;
#ifdef _WIN32
        const auto address = GetProcAddress(module_, name);
        Proc proc{};
        static_assert(sizeof(proc) == sizeof(address));
        std::memcpy(&proc, &address, sizeof(proc));
        return proc;
#else
        return reinterpret_cast<Proc>(dlsym(module_, name));
#endif
    }

private:
#ifdef _WIN32
    HMODULE module_{};
#else
    void* module_{};
#endif
};

bool linked() { return panel.connection == Connection::live; }
bool pending() { return panel.pending != 0; }

void report(Result result) {
    panel.show_result = true;
    panel.result = result;
}
void poll() {
    const auto now = Clock::now();
    if (now < panel.next_poll) return;
    panel.next_poll = now + std::chrono::milliseconds(pending() ? 40 : 150);
    const CoreModule module;
    const auto read = module.get<Reader>(export_name);
    const auto session = panel.view.session;
    if (!module) {
        panel.connection = Connection::missing;
    } else if (!read || !module.get<Sender>(command_export)) {
        panel.connection = Connection::old_core;
    } else {
        switch (read(&panel.view, sizeof(Snapshot))) {
            case Read::ready: panel.connection = Connection::live; break;
            case Read::busy:
                if (!linked()) panel.connection = Connection::busy;
                break;
            case Read::inactive: panel.connection = Connection::inactive; break;
            case Read::bad_size: panel.connection = Connection::old_core; break;
        }
    }
    if (!linked() || panel.view.session != session) {
        if (pending()) report(Result::stale);
        panel.pending = 0;
        panel.binding = -1;
        panel.dragging_volume = false;
        panel.editing_approach = false;
        panel.reset_confirm = false;
    } else if (pending()) {
        if (panel.view.ticket == panel.pending) {
            report(panel.view.command_result);
            panel.pending = 0;
        } else if (now - panel.sent_at >= std::chrono::seconds(5)) {
            report(Result::unavailable);
            panel.pending = 0;
        }
    }
}

bool send(Operation operation, unsigned value = 0, const char* text = "") {
    if (!linked() || pending()) return false;
    const CoreModule module;
    const auto write = module.get<Sender>(command_export);
    if (!write) {
        report(Result::inactive);
        return false;
    }
    static std::uint64_t sequence = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
    Command command{panel.view.session, ++sequence, panel.view.route_revision, operation, value, {}};
    const auto length = std::min(std::strlen(text), sizeof(command.text) - 1);
    std::memcpy(command.text, text, length);
    const auto result = write(&command, sizeof(command));
    report(result);
    if (result != Result::accepted) return false;
    panel.pending = command.ticket;
    panel.sent_at = Clock::now();
    panel.next_poll = {};
    return true;
}

void text(const std::string& value) { panel.ui->UI_TextWrapped(value.c_str()); }
const char* label(const char* english, const char* chinese) { return panel.label(english, chinese); }

void detail(const char* key, const std::string& value) {
    text(std::string(key) + (panel.zh ? "：" : ": ") + value);
}

const char* absent() { return label("Unavailable", "暂无"); }

std::string station(const Station& stop) {
    if (!stop.name[0]) {
        return stop.id[0] ? stop.id : absent();
    }
    return stop.name;
}

const char* action(Kind kind) {
    switch (kind) {
        case Kind::welcome: return label("Welcome", "发车欢迎");
        case Kind::next: return label("Next stop", "下一站");
        case Kind::approaching: return label("Approaching", "即将到站");
        case Kind::finish: return label("Terminal", "终点结束");
        case Kind::none: return label("None", "无");
    }
    return absent();
}

const char* outcome(Outcome value) {
    switch (value) {
        case Outcome::stopped: return label("Stopped manually", "手动停止");
        case Outcome::no_route: return label("No broadcast target", "没有播报目标");
        case Outcome::missing_audio: return label("Playable audio missing or unreadable", "音频缺失或不可读取");
        case Outcome::muted: return label("Muted", "静音");
        case Outcome::audio_error: return label("Audio device error", "音频设备错误");
        case Outcome::playing_named: return label("City-specific WAV started", "已开始播放城市专用整段音频");
        case Outcome::playing_assembled: return label("Assembled audio started", "已开始播放拼接音频");
        case Outcome::playing_generic: return label("Generic fallback started", "已开始播放通用语音");
        case Outcome::playing_multilingual: return label("Four-language audio started", "已开始播放四语音频");
        case Outcome::none: return label("No attempts this session", "本次会话尚未尝试");
    }
    return absent();
}

const char* connection() {
    switch (panel.connection) {
        case Connection::missing: return label("CoachAnnouncer.dll is not loaded", "未载入 CoachAnnouncer.dll");
        case Connection::old_core: return label("Install the matching CoachAnnouncer core", "请安装同版本 CoachAnnouncer 主插件");
        case Connection::inactive: return label("Waiting for the SCS telemetry session", "正在等待 SCS Telemetry 会话");
        case Connection::busy: return label("Waiting for the current snapshot", "正在等待读取状态");
        case Connection::live:
            return panel.view.paused ? label("Connected · paused", "已连接 · 暂停")
                                     : label("Connected · running", "已连接 · 运行中");
    }
    return absent();
}

struct Color { float r, g, b, a; };
constexpr Color mint{.40f, .88f, .77f, 1};
constexpr Color muted{.66f, .74f, .80f, 1};
constexpr Color amber{1, .73f, .40f, 1};

void style(SPF_StyleColor slot, Color c) { panel.ui->UI_PushStyleColor(slot, c.r, c.g, c.b, c.a); }
void colored(const std::string& value, Color c) { panel.ui->UI_TextColored(c.r, c.g, c.b, c.a, value.c_str()); }
void same() { panel.ui->UI_SameLine(0, -1); }

struct Theme {
    explicit Theme(SPF_UI_API* ui) : ui(ui) {
        const float height = ui->UI_GetTextLineHeight();
        ui->UI_PushStyleVarVec2(SPF_STYLE_VAR_FRAME_PADDING, height * .65f, height * .42f);
        ui->UI_PushStyleVarVec2(SPF_STYLE_VAR_ITEM_SPACING, height * .65f, height * .53f);
        ui->UI_PushStyleVarFloat(SPF_STYLE_VAR_FRAME_ROUNDING, height * .36f);
        ui->UI_PushStyleVarFloat(SPF_STYLE_VAR_TAB_ROUNDING, height * .36f);
        ui->UI_PushStyleVarFloat(SPF_STYLE_VAR_CHILD_ROUNDING, height * .58f);
        style(SPF_COLOR_CHILD_BG, {.08f,.12f,.16f,.96f});
        style(SPF_COLOR_BORDER, {.22f,.32f,.38f,1});
        style(SPF_COLOR_BUTTON, {.17f,.26f,.32f,1});
        style(SPF_COLOR_BUTTON_HOVERED, {.23f,.38f,.43f,1});
        style(SPF_COLOR_BUTTON_ACTIVE, {.19f,.47f,.45f,1});
        style(SPF_COLOR_FRAME_BG, {.14f,.22f,.27f,1});
        style(SPF_COLOR_CHECK_MARK, mint);
        style(SPF_COLOR_SLIDER_GRAB, mint);
        style(SPF_COLOR_PLOT_HISTOGRAM, {.22f,.55f,.51f,1});
        style(SPF_COLOR_TAB_ACTIVE, {.16f,.31f,.34f,1});
    }
    ~Theme() { ui->UI_PopStyleColor(10); ui->UI_PopStyleVar(5); }
    SPF_UI_API* ui;
};

enum class Icon { coach, play, stop, sound, flag, route, gear };

void icon(Icon glyph, float x, float y, float size, std::uint32_t color) {
    auto* ui = panel.ui;
    auto dl = ui->UI_GetWindowDrawList();
    const auto line = [&](float a, float b, float c, float d) {
        ui->UI_DrawList_AddLine(dl, x+a*size, y+b*size, x+c*size, y+d*size, color, std::max(1.0f,size*.11f));
    };
    const auto dot = [&](float a, float b, float radius) {
        ui->UI_DrawList_AddCircleFilled(dl, x+a*size, y+b*size, radius*size, color, 0);
    };
    if (glyph == Icon::play) {
        ui->UI_DrawList_AddTriangleFilled(dl, x+.2f*size, y+.07f*size, x+.88f*size, y+.5f*size,
                                         x+.2f*size, y+.93f*size, color);
    } else if (glyph == Icon::stop) {
        ui->UI_DrawList_AddRectFilled(dl, x+.15f*size, y+.15f*size, x+.85f*size, y+.85f*size, color, 1, SPF_DRAW_FLAG_NONE);
    } else if (glyph == Icon::coach) {
        ui->UI_DrawList_AddRect(dl, x+.07f*size, y+.08f*size, x+.93f*size, y+.76f*size,
                               color, size*.15f, SPF_DRAW_FLAG_NONE, std::max(1.0f,size*.085f));
        line(.14f,.5f,.86f,.5f); line(.5f,.15f,.5f,.5f);
        dot(.23f,.84f,.11f); dot(.77f,.84f,.11f);
    } else if (glyph == Icon::route) {
        line(.22f,.78f,.74f,.21f);
        dot(.22f,.78f,.19f); dot(.74f,.21f,.19f);
    } else if (glyph == Icon::flag) {
        line(.18f,.08f,.18f,.93f); line(.18f,.13f,.82f,.13f);
        line(.82f,.13f,.62f,.37f); line(.62f,.37f,.18f,.37f);
    } else if (glyph == Icon::sound) {
        line(.22f,.33f,.51f,.12f); line(.51f,.12f,.51f,.88f);
        line(.51f,.88f,.22f,.67f); line(.22f,.67f,.10f,.67f);
        line(.10f,.67f,.10f,.33f); line(.10f,.33f,.22f,.33f);
        line(.70f,.24f,.88f,.5f); line(.88f,.5f,.70f,.76f);
    } else {
        ui->UI_DrawList_AddCircle(dl, x+.5f*size, y+.5f*size, size*.26f, color, 0, std::max(1.0f,size*.10f));
        line(.5f,.05f,.5f,.20f); line(.5f,.80f,.5f,.95f);
        line(.05f,.5f,.20f,.5f); line(.80f,.5f,.95f,.5f);
    }
}

void heading(Icon glyph, const char* title) {
    auto* ui = panel.ui;
    float x{}, y{};
    ui->UI_GetCursorScreenPos(&x, &y);
    const auto unit = ui->UI_GetTextLineHeight();
    ui->UI_Dummy(unit, unit);
    icon(glyph, x, y, unit, 0xffc4e066);
    same();
    colored(title, mint);
}

bool tile(const char* title, Icon glyph, float width, bool enabled, bool selected = false) {
    auto* ui = panel.ui;
    const float height = ui->UI_GetTextLineHeight() * 2.55f;
    ui->UI_BeginDisabled(!enabled);
    if (selected) style(SPF_COLOR_BUTTON, {.15f,.39f,.39f,1});
    const bool pressed = ui->UI_Button((std::string("   ") + title).c_str(), width, height);
    float x{}, y{}, right{}, bottom{}, text_height{}, full_width{};
    ui->UI_GetItemRectMin(&x, &y);
    ui->UI_GetItemRectMax(&right, &bottom);
    ui->UI_CalcTextSize((std::string("   ") + title).c_str(), &full_width, &text_height);
    const auto scale = ui->UI_GetTextLineHeight();
    const auto size = scale*.8f;
    const auto icon_x = x + (right - x - full_width)*.5f - scale*.47f;
    if (icon_x >= x + 2) {
        icon(glyph, icon_x, y + (bottom - y - size)*.5f, size, enabled ? 0xffc4e066 : 0xff7f8880);
    }
    if (selected) ui->UI_PopStyleColor(1);
    ui->UI_EndDisabled();
    return enabled && pressed;
}

float width() {
    float w{}, h{};
    panel.ui->UI_GetContentRegionAvail(&w, &h);
    return w;
}

void banner() {
    auto* ui = panel.ui;
    const auto line = ui->UI_GetTextLineHeightWithSpacing();
    if (ui->UI_BeginChild("##coach-banner", 0, line*2.55f, true, SPF_WINDOW_FLAG_NO_SCROLLBAR)) {
        heading(Icon::coach, label("CoachAnnouncer", "CoachAnnouncer 客运广播"));
        if (width() > line*21) {
            same();
            colored(connection(), linked() ? mint : amber);
        } else {
            colored(connection(), linked() ? mint : amber);
        }
    }
    ui->UI_EndChild();
}

void feedback() {
    if (!panel.show_result) return;
    const char* msg = "";
    switch (panel.result) {
        case Result::accepted: msg = label("Applying…", "正在应用…"); break;
        case Result::applied: msg = label("Applied", "已应用"); break;
        case Result::busy: msg = label("CoachAnnouncer is busy. Try again.", "广播核心正在处理操作，请重试。"); break;
        case Result::inactive: msg = label("CoachAnnouncer disconnected.", "广播核心连接已断开。"); break;
        case Result::stale: msg = label("The route or session changed. Try again.", "线路或会话已变化，请重试。"); break;
        case Result::invalid: msg = label("Invalid selection or shortcut conflict.", "选项无效或快捷键冲突。"); break;
        case Result::write_failed: msg = label("Could not save settings.ini.", "无法保存 settings.ini。"); break;
        case Result::unavailable: msg = label("Action unavailable or timed out.", "操作暂不可用或已超时。"); break;
    }
    colored(msg, panel.result == Result::applied ? mint : amber);
}

void stop_card(const char* id, const char* title, const Station& stop, Icon glyph, float w) {
    auto* ui = panel.ui;
    const float row = ui->UI_GetTextLineHeightWithSpacing();
    if (ui->UI_BeginChild(id, w, row*3.05f, true, SPF_WINDOW_FLAG_NONE)) {
        heading(glyph, title);
        text(station(stop));
    }
    ui->UI_EndChild();
}

void timeline(const Snapshot& v, unsigned count) {
    auto* ui = panel.ui;
    heading(Icon::route, label("Stops", "经停站"));
    const auto row = ui->UI_GetTextLineHeightWithSpacing();
    const auto dot = [&](const Station& stop, bool active, bool more) {
        float x{}, y{};
        ui->UI_GetCursorScreenPos(&x, &y);
        ui->UI_Dummy(row, row*.72f);
        auto dl = ui->UI_GetWindowDrawList();
        if (more) ui->UI_DrawList_AddLine(dl, x+row*.46f, y+row*.33f, x+row*.46f,
                                         y+row*1.36f, 0xff566d70, 1.5f);
        ui->UI_DrawList_AddCircleFilled(dl, x+row*.46f, y+row*.33f, row*(active?.18f:.12f),
                                        active ? 0xffc4e066 : 0xff8c9ba0, 0);
        same();
        text(station(stop));
    };
    dot(v.stages[0].source, false, true);
    for (unsigned i = 0; i < count; ++i) {
        if (i && v.stages[i].source.id[0] &&
            std::strcmp(v.stages[i].source.id, v.stages[i-1].destination.id) != 0) {
            dot(v.stages[i].source, false, true);
        }
        dot(v.stages[i].destination, i == v.stage_current, i+1 < count);
    }
}

void route_tab() {
    auto* ui = panel.ui;
    if (!linked()) {
        text(connection());
        return;
    }
    const auto& v = panel.view;
    if (v.route == RouteState::waiting || v.route == RouteState::none) {
        heading(Icon::route, v.route == RouteState::waiting
            ? label("Waiting for bus_job data.", "正在等待客运合同数据。")
            : label("No active passenger contract.", "当前没有客运合同。"));
        return;
    }

    const auto market = std::string_view(v.market);
    text(market == "company_job" ? label("Company service", "公司客运")
         : market == "tourist_job" ? label("Tourist service", "旅游客运") : v.market);
    const auto leg = v.stage_current != unknown && v.stage_current < v.stage_count;
    if (leg && v.stage_count && v.stage_count <= max_stages) {
        const auto progress = std::to_string(v.stage_current + 1) + " / " + std::to_string(v.stage_count);
        ui->UI_ProgressBar(static_cast<float>(v.stage_current+1)/v.stage_count, -1, 0, progress.c_str());
    }
    const auto available = width();
    const auto unit = ui->UI_GetTextLineHeight();
    const bool columns = available > unit*25;
    const auto card_width = columns ? (available-unit*.65f)*.5f : 0.0f;
    stop_card("##next-card", label("Next stop", "下一站"), v.next, Icon::route, card_width);
    if (columns) same();
    stop_card("##terminal-card", label("Terminal", "终点"), v.terminal, Icon::flag, card_width);
    const bool overridden = v.override_stop.id[0] != '\0';
    if (overridden) detail(label("Manual target", "手动播报目标"), station(v.override_stop));
    if (v.route == RouteState::incomplete) {
        colored(label("Route data incomplete", "线路数据不完整"), amber);
    }

    const auto count = v.stage_count == unknown ? 0 : std::min(v.stage_count, max_stages);
    if (leg && v.next.id[0] && v.stage_count <= max_stages) {
        std::vector<unsigned> indices;
        std::vector<std::string> names{label("Use game route", "跟随游戏线路")};
        int selected = 0;
        for (unsigned i = v.stage_current; i < count; ++i) {
            const auto& stop = v.stages[i].destination;
            if (!stop.id[0]) continue;
            indices.push_back(i);
            names.push_back(station(stop));
            if (overridden && std::strcmp(stop.id, v.override_stop.id) == 0) selected = static_cast<int>(indices.size());
        }
        std::vector<const char*> choices;
        for (const auto& name : names) choices.push_back(name.c_str());
        ui->UI_BeginDisabled(pending());
        ui->UI_SetNextItemWidth(-1);
        if (ui->UI_Combo("##stop-choice", &selected, choices.data(), static_cast<int>(choices.size()))) {
            if (selected == 0) send(Operation::clear_override);
            else if (selected > 0 && static_cast<std::size_t>(selected) <= indices.size()) {
                const unsigned i = indices[selected-1];
                send(Operation::override_stop, i, v.stages[i].destination.id);
            }
        }
        ui->UI_EndDisabled();
        text(label("Manual next / approaching · current leg only", "仅影响当前区间的手动「下一站／即将到站」"));
    }
    if (count) timeline(v, count);
}

void broadcast_tab() {
    auto* ui = panel.ui;
    const auto& a = panel.view.audio;
    if (!linked() || !a.available) {
        text(linked() ? label("Audio engine unavailable", "音频引擎不可用") : connection());
        return;
    }
    heading(Icon::sound, label("Listen", "试听广播"));
    const auto tile_width = std::max(1.0f, (width()-ui->UI_GetTextLineHeight()*.65f)*.5f);
    for (int i = 0; i < 4; ++i) {
        if (i % 2) same();
        if (tile(action(static_cast<Kind>(i+1)), Icon::play, tile_width, !pending(),
                 a.playing == static_cast<Kind>(i+1))) send(Operation::preview, i);
    }
    if (tile(label("Stop playback", "停止播放"), Icon::stop, width(), !pending())) send(Operation::stop);
    if (ui->UI_BeginChild("##playback", 0, ui->UI_GetTextLineHeightWithSpacing()*2.8f,
                          true, SPF_WINDOW_FLAG_NONE)) {
        if (a.playing == Kind::none) {
            heading(Icon::sound, label("Idle", "待机"));
        } else {
            const std::string now = std::string(a.previewing ? label("Previewing  ·  ", "正在试听  ·  ") :
                                          label("Playing  ·  ", "正在播放  ·  ")) + action(a.playing);
            heading(Icon::sound, now.c_str());
        }
        if (a.last_result != Outcome::none) text(outcome(a.last_result));
    }
    ui->UI_EndChild();

    heading(Icon::gear, label("Language and volume", "语言与音量"));
    const char* choices[]{label("All four languages", "四语顺播"), "Deutsch", "English", "普通话", "日本語"};
    constexpr const char* voices[]{"Katja · Sonia · Xiaoxiao · Nanami", "de-DE-KatjaNeural",
        "en-GB-SoniaNeural", "zh-CN-XiaoxiaoNeural", "ja-JP-NanamiNeural"};
    int selected = 0;
    for (int i = 0; i < 5; ++i) if (std::strcmp(a.language, languages[i]) == 0) selected = i;
    ui->UI_BeginDisabled(pending());
    const auto span = width();
    const auto unit = ui->UI_GetTextLineHeight();
    const bool columns = span > unit*24;
    ui->UI_SetNextItemWidth(columns ? span*.54f : -1.0f);
    if (ui->UI_Combo("##language", &selected, choices, 5)) send(Operation::language, selected);
    if (columns) same();
    if (!panel.dragging_volume) panel.volume = static_cast<int>(a.volume);
    ui->UI_SetNextItemWidth(-1);
    const bool volume_changed = ui->UI_SliderInt("##volume", &panel.volume, 0, 100, "%d%%", SPF_SLIDER_FLAG_ALWAYS_CLAMP);
    const bool volume_finished = ui->UI_IsItemDeactivatedAfterEdit();
    panel.dragging_volume = ui->UI_IsItemActive();
    if (volume_finished || (volume_changed && !panel.dragging_volume)) send(Operation::volume, panel.volume);
    if (selected >= 0 && selected < 5) colored(voices[selected], muted);
    ui->UI_EndDisabled();
}

std::string version(std::uint32_t v) {
    return std::to_string(v >> 16) + "." + std::to_string(v & 0xffffu);
}

void settings_tab() {
    auto* ui = panel.ui;
    heading(Icon::gear, label("Shortcuts", "快捷键"));
    const auto* keys = panel.core ? panel.core->keybinds : nullptr;
    if (keys && panel.keys) {
        const auto count = keys->Kbind_GetBindingCount(panel.keys, action_id);
        char binding[256]{};
        if (count > 0) keys->Kbind_GetBindingDisplayName(panel.keys, action_id, 0, binding, sizeof(binding));
        if (ui->UI_Button(label("Change panel shortcut", "修改面板快捷键"), 0, 0)) {
            keys->Kbind_OpenRebindPopup(panel.keys, action_id, count > 0 ? 0 : -1);
        }
        same();
        colored(binding[0] ? binding : label("Not assigned", "尚未绑定"), muted);
    }
    if (!linked()) return;
    const auto& v = panel.view;
    const auto& a = v.audio;
    ui->UI_BeginDisabled(pending());
    for (int i = 0; i < 4; ++i) {
        const auto name = std::string(action(static_cast<Kind>(i+1))) + "   [" + a.bindings[i] + "]"
            + "###binding" + std::to_string(i);
        if (ui->UI_Button(name.c_str(), 0, 0)) {
            panel.binding = i;
            std::snprintf(panel.chord, sizeof(panel.chord), "%s", a.bindings[i]);
        }
        if (panel.binding == i) {
            ui->UI_SetNextItemWidth(std::max(90.0f, width()*.53f));
            ui->UI_InputTextWithHint("##chord", "Ctrl+Num2 / Off", panel.chord, sizeof(panel.chord), SPF_INPUT_TEXT_FLAG_NONE);
            same();
            if (ui->UI_Button(label("Save", "保存"), 0, 0) && send(Operation::binding, i, panel.chord)) panel.binding = -1;
            same();
            if (ui->UI_Button(label("Cancel", "取消"), 0, 0)) panel.binding = -1;
        }
    }

    heading(Icon::flag, label("Automatic announcements", "自动广播"));
    bool next = a.auto_next != 0, finish = a.auto_finish != 0, approach = a.approach_metres != 0;
    if (ui->UI_Checkbox(label("Next stop on departure", "发车后播报下一站"), &next)) send(Operation::auto_next, next);
    if (ui->UI_Checkbox(label("Announce terminal before arrival", "抵达前播报终点"), &finish)) send(Operation::auto_finish, finish);
    if (ui->UI_Checkbox(label("Enable pre-arrival distance", "启用到站前距离触发"), &approach)) {
        send(Operation::approach, approach ? 1000 : 0);
    }
    if (approach) {
        text(label("Intermediate stops: scaled estimate · Terminal: GPS", "途经站按地图比例估算，终点优先使用导航路程"));
        if (!panel.editing_approach) panel.approach = a.approach_metres ? static_cast<int>(a.approach_metres) : 1000;
        ui->UI_SetNextItemWidth(-1);
        const bool approach_changed = ui->UI_SliderInt("##threshold", &panel.approach, 300, 25000, "%d m", SPF_SLIDER_FLAG_ALWAYS_CLAMP);
        const bool approach_finished = ui->UI_IsItemDeactivatedAfterEdit();
        panel.editing_approach = ui->UI_IsItemActive();
        if (approach_finished || (approach_changed && !panel.editing_approach)) send(Operation::approach, panel.approach);
    }

    heading(Icon::sound, label("Audio library", "音频资源"));
    const auto* assets = a.assets == Assets::present ? label("Folder found", "目录可用")
        : a.assets == Assets::missing ? label("Folder missing", "缺少目录") : label("Scan unavailable", "无法扫描目录");
    text(std::string(assets) + "  ·  " + std::to_string(a.station_files) + " WAV");
    if (ui->UI_Button(label("Rescan audio", "重新扫描音频"), 0, 0)) send(Operation::refresh_audio);
    if (v.audio_path[0]) {
        same();
        if (ui->UI_Button(label("Copy audio path", "复制音频路径"), 0, 0)) ui->UI_SetClipboardText(v.audio_path);
    }

    heading(Icon::gear, label("Diagnostics", "诊断"));
    text(std::string(v.version) + "  ·  SCS API " + version(v.api_version) + "  ·  SDK " + version(v.game_sdk_version));
    if (a.invalid_config_entries) detail(label("Invalid settings", "无效配置条目"), std::to_string(a.invalid_config_entries));
    if (v.log_path[0] && ui->UI_Button(label("Copy log path", "复制日志路径"), 0, 0)) ui->UI_SetClipboardText(v.log_path);
    if (!panel.reset_confirm) {
        if (ui->UI_Button(label("Restore broadcast defaults…", "恢复广播默认设置…"), 0, 0)) panel.reset_confirm = true;
    } else {
        text(label("Restore language, volume, automatic settings and broadcast shortcuts?", "恢复语言、音量、自动广播和四个播报快捷键？"));
        if (ui->UI_Button(label("Restore", "确认恢复"), 0, 0) && send(Operation::reset)) panel.reset_confirm = false;
        same();
        if (ui->UI_Button(label("Keep settings", "保留当前设置"), 0, 0)) panel.reset_confirm = false;
    }
    ui->UI_EndDisabled();
}

void draw(SPF_UI_API* ui, void*) {
    poll();
    panel.zh = false;
    if (panel.core && panel.core->localization && panel.localization) {
        char locale[8]{};
        panel.core->localization->Loc_GetString(panel.localization, "panel.language", locale, sizeof(locale));
        panel.zh = std::strcmp(locale, "zh") == 0;
    }
    const Theme theme(ui);
    banner();
    feedback();
    if (!ui->UI_BeginTabBar("##coach_tabs", SPF_TAB_BAR_FLAG_NONE)) return;
    if (ui->UI_BeginTabItem(label("Route###route", "线路###route"), nullptr, SPF_TAB_ITEM_FLAG_NONE)) {
        route_tab();
        ui->UI_EndTabItem();
    }
    if (ui->UI_BeginTabItem(label("Broadcast###broadcast", "广播###broadcast"), nullptr, SPF_TAB_ITEM_FLAG_NONE)) {
        broadcast_tab();
        ui->UI_EndTabItem();
    }
    if (ui->UI_BeginTabItem(label("Settings###settings", "设置###settings"), nullptr, SPF_TAB_ITEM_FLAG_NONE)) {
        settings_tab();
        ui->UI_EndTabItem();
    }
    ui->UI_EndTabBar();
}

void toggle() { toggle_requests.fetch_xor(1, std::memory_order_relaxed); }

void on_update() {
    if (toggle_requests.exchange(0, std::memory_order_relaxed) && panel.ui && panel.window) {
        const bool show = !panel.ui->UI_IsVisible(panel.window);
        panel.ui->UI_SetVisibility(panel.window, show);
        if (show) {
            panel.next_poll = {};
            panel.ui->UI_SetFocus(panel.window);
        }
    }
}

void build_manifest(SPF_Manifest_Builder_Handle* builder, const SPF_Manifest_Builder_API* api) {
    api->Info_SetName(builder, plugin_name);
    api->Info_SetVersion(builder, COACH_PANEL_VERSION);
    api->Info_SetAuthor(builder, "CoachAnnouncer");
    api->Info_SetDescriptionKey(builder, "plugin.description");
    api->Info_SetMinFrameworkVersion(builder, "1.2.6");

    api->Policy_SetAllowUserConfig(builder, true);
    api->Policy_AddConfigurableSystem(builder, "ui");
    api->Policy_AddConfigurableSystem(builder, "localization");
    api->Defaults_SetLocalization(builder, "en");
    api->Defaults_AddWindow(builder, window_id, false, true, 110, 110, 650, 560, false, false);
    api->Defaults_AddKeybind(builder, "Panel", "toggle", "chord",
        "keyboard:KEY_LCONTROL+keyboard:KEY_LSHIFT+keyboard:KEY_F8", "always");
    api->Meta_AddWindow(builder, window_id, "panel.title", "panel.description");
    api->Meta_AddKeybind(builder, "Panel", "toggle", "shortcut.title", "shortcut.description");
}

void on_load(const SPF_Load_API*) {}

void on_activated(const SPF_Core_API* core) {
    panel.core = core;
    if (core->localization) panel.localization = core->localization->Loc_GetContext(plugin_name);
    if (core->keybinds) {
        panel.keys = core->keybinds->Kbind_GetContext(plugin_name);
        if (panel.keys) core->keybinds->Kbind_Register(panel.keys, action_id, toggle);
    }
}

void on_register_ui(SPF_UI_API* ui) {
    panel.ui = ui;
    ui->UI_RegisterDrawCallback(plugin_name, window_id, draw, nullptr);
    if (!panel.window) {
        panel.window = ui->UI_GetWindowHandle(plugin_name, window_id);
        if (panel.window) ui->UI_SetVisibility(panel.window, false);
    }
}

void on_unload() {
    panel = {};
    toggle_requests.store(0, std::memory_order_relaxed);
}
} // namespace

extern "C" SPF_PLUGIN_EXPORT bool SPF_GetManifestAPI(SPF_Manifest_API* api) {
    if (!api) return false;
    api->BuildManifest = build_manifest;
    return true;
}

extern "C" SPF_PLUGIN_EXPORT bool SPF_GetPlugin(SPF_Plugin_Exports* exports) {
    if (!exports) return false;
    exports->OnLoad = on_load;
    exports->OnActivated = on_activated;
    exports->OnRegisterUI = on_register_ui;
    exports->OnUpdate = on_update;
    exports->OnUnload = on_unload;
    return true;
}
