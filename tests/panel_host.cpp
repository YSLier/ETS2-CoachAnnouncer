#include "panel_snapshot.hpp"

#include <SPF_KeyBinds_API.h>
#include <SPF_Manifest_API.h>
#include <SPF_Plugin.h>
#include <SPF_UI_API.h>
#include <scssdk_telemetry.h>

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <dlfcn.h>

namespace {
using namespace std::chrono_literals;
using coach::panel::Read;
using coach::panel::Snapshot;
using coach::panel::Command;
using coach::panel::Operation;
using coach::panel::Result;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Module {
    explicit Module(const char* path) : handle(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {
        if (!handle) throw std::runtime_error(dlerror());
    }
    ~Module() { dlclose(handle); }
    template<class Fn>
    Fn get(const char* name) const {
        const auto result = reinterpret_cast<Fn>(dlsym(handle, name));
        check(result != nullptr, name);
        return result;
    }
    void* handle;
};

struct SPFMock {
    bool visible{true}; // Simulate persisted open state from a prior game run.
    bool focused{};
    bool click_change{};
    bool rebind_called{};
    bool choose_stop{};
    bool change_auto{};
    bool disable{};
    unsigned dots{};
    void (*toggle)(){};
    SPF_DrawCallback draw{};
    std::string rendered;
} spf;

struct SdkCallbacks {
    scs_telemetry_event_callback_t events[7]{};
    scs_context_t context{};
} sdk;

SCSAPI_RESULT register_event(scs_event_t event, scs_telemetry_event_callback_t callback,
                            scs_context_t context) {
    sdk.events[event] = callback;
    sdk.context = context;
    return SCS_RESULT_ok;
}

SCSAPI_RESULT unregister_event(scs_event_t event) {
    sdk.events[event] = nullptr;
    return SCS_RESULT_ok;
}

scs_named_value_t str(const char* key, const char* value, scs_u32_t index = SCS_U32_NIL) {
    scs_named_value_t field{};
    field.name = key;
    field.index = index;
    field.value.type = SCS_VALUE_TYPE_string;
    field.value.value_string.value = value;
    return field;
}

scs_named_value_t u32(const char* key, scs_u32_t value) {
    scs_named_value_t field{};
    field.name = key;
    field.index = SCS_U32_NIL;
    field.value.type = SCS_VALUE_TYPE_u32;
    field.value.value_u32.value = value;
    return field;
}

void set_stop(scs_u32_t current) {
    // Station fields transcribed from an actual ETS2 1.62.3.3s bus_job log.
    const scs_named_value_t bus[]{
        str("bus_job.market", "company_job"),
        u32("bus.stages.count", 2), u32("bus.stages.current", current),
        str("source_stop.id", "helsinki", 0), str("source_stop.name", "赫尔辛基", 0),
        str("destination_stop.id", "tallinn", 0), str("destination_stop.name", "塔林", 0),
        str("source_stop.id", "tallinn", 1), str("source_stop.name", "塔林", 1),
        str("destination_stop.id", "parnu", 1), str("destination_stop.name", "派尔努", 1),
        {}
    };
    const scs_telemetry_configuration_t cfg{"bus_job", bus};
    sdk.events[SCS_TELEMETRY_EVENT_configuration](SCS_TELEMETRY_EVENT_configuration, &cfg, sdk.context);
}

SPF_UI_API mock_ui() {
    SPF_UI_API ui{};
    ui.UI_RegisterDrawCallback = [](const char* plugin, const char* window, SPF_DrawCallback cb, void*) {
        check(std::strcmp(plugin, "CoachAnnouncerPanel") == 0, "unexpected owner");
        check(std::strcmp(window, "QuickSettings") == 0, "unexpected window ID");
        spf.draw = cb;
    };
    ui.UI_GetWindowHandle = [](const char*, const char*) {
        return reinterpret_cast<SPF_Window_Handle*>(&spf);
    };
    ui.UI_SetVisibility = [](SPF_Window_Handle*, bool show) { spf.visible = show; };
    ui.UI_IsVisible = [](SPF_Window_Handle*) { return spf.visible; };
    ui.UI_SetFocus = [](SPF_Window_Handle*) { spf.focused = true; };
    ui.UI_BeginTabBar = [](const char*, SPF_TabBarFlags) { return true; };
    ui.UI_EndTabBar = [] {};
    ui.UI_BeginTabItem = [](const char* title, bool*, SPF_TabItemFlags) {
        spf.rendered += title;
        return true;
    };
    ui.UI_EndTabItem = [] {};
    ui.UI_TextWrapped = [](const char* text) { spf.rendered += std::string(text) + '\n'; };
    ui.UI_TextColored = [](float, float, float, float, const char* text) {
        spf.rendered += std::string(text) + '\n';
    };
    ui.UI_Button = [](const char* text, float, float) {
        return spf.click_change && std::strcmp(text, "Change panel shortcut") == 0;
    };
    ui.UI_GetTextLineHeight = [] { return 16.0f; };
    ui.UI_GetTextLineHeightWithSpacing = [] { return 24.0f; };
    ui.UI_PushStyleVarVec2 = [](SPF_StyleVar, float, float) {};
    ui.UI_PushStyleVarFloat = [](SPF_StyleVar, float) {};
    ui.UI_PushStyleColor = [](SPF_StyleColor, float, float, float, float) {};
    ui.UI_PopStyleVar = [](int) {};
    ui.UI_PopStyleColor = [](int) {};
    ui.UI_BeginChild = [](const char*, float, float, bool, SPF_WindowFlags) { return true; };
    ui.UI_EndChild = [] {};
    ui.UI_GetContentRegionAvail = [](float* w, float* h) { *w = 570; *h = 480; };
    ui.UI_GetCursorScreenPos = [](float* x, float* y) { *x = *y = 0; };
    ui.UI_GetWindowDrawList = [] { return reinterpret_cast<SPF_DrawList_Handle>(&spf); };
    ui.UI_Dummy = [](float, float) {};
    ui.UI_SameLine = [](float, float) {};
    ui.UI_DrawList_AddLine = [](SPF_DrawList_Handle, float, float, float, float, std::uint32_t, float) {};
    ui.UI_DrawList_AddCircleFilled = [](SPF_DrawList_Handle, float, float, float, std::uint32_t, int) { ++spf.dots; };
    ui.UI_DrawList_AddCircle = [](SPF_DrawList_Handle, float, float, float, std::uint32_t, int, float) {};
    ui.UI_DrawList_AddRect = [](SPF_DrawList_Handle, float, float, float, float, std::uint32_t, float, SPF_DrawFlags, float) {};
    ui.UI_ProgressBar = [](float, float, float, const char*) {};
    ui.UI_BeginDisabled = [](bool disabled) { spf.disable = disabled; };
    ui.UI_EndDisabled = [] { spf.disable = false; };
    ui.UI_SetNextItemWidth = [](float) {};
    ui.UI_Combo = [](const char* id, int* index, const char* const[], int count) {
        if (spf.disable || !spf.choose_stop || std::strcmp(id, "##stop-choice") != 0) return false;
        check(count == 3, "unexpected station choices");
        *index = 2;
        spf.choose_stop = false;
        return true;
    };
    ui.UI_Checkbox = [](const char* id, bool* checked) {
        if (spf.disable || !spf.change_auto || std::strcmp(id, "Next stop on departure") != 0) return false;
        *checked = false;
        spf.change_auto = false;
        return true;
    };
    ui.UI_SliderInt = [](const char*, int*, int, int, const char*, SPF_SliderFlags) { return false; };
    ui.UI_IsItemActive = [] { return false; };
    ui.UI_IsItemDeactivatedAfterEdit = [] { return false; };
    return ui;
}

SPF_KeyBinds_API mock_keys() {
    SPF_KeyBinds_API keys{};
    keys.Kbind_GetContext = [](const char*) { return reinterpret_cast<SPF_KeyBinds_Handle*>(&spf); };
    keys.Kbind_Register = [](SPF_KeyBinds_Handle*, const char* action, void (*cb)()) {
        check(std::strcmp(action, "Panel.toggle") == 0, "wrong action ID");
        spf.toggle = cb;
    };
    keys.Kbind_GetBindingCount = [](SPF_KeyBinds_Handle*, const char*) { return 1; };
    keys.Kbind_GetBindingDisplayName = [](SPF_KeyBinds_Handle*, const char*, int, char* out, int size) {
        const char text[] = "Left Ctrl + Left Shift + F8";
        check(size >= sizeof(text), "binding buffer too small");
        std::memcpy(out, text, sizeof(text));
        return static_cast<int>(sizeof(text) - 1);
    };
    keys.Kbind_OpenRebindPopup = [](SPF_KeyBinds_Handle*, const char* action, int index) {
        check(std::strcmp(action, "Panel.toggle") == 0 && index == 0, "wrong rebind target");
        spf.rebind_called = true;
    };
    return keys;
}

void rendered(SPF_UI_API& ui, const char* expected) {
    check(spf.visible && spf.draw, "window must be visible");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        spf.rendered.clear();
        spf.draw(&ui, nullptr);
        if (spf.rendered.find(expected) != std::string::npos) return;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    std::cerr << spf.rendered << '\n';
    throw std::runtime_error(expected);
}
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3, "expected core and panel module paths");
        Module panel(argv[2]);
        SPF_Manifest_API manifest{};
        check(panel.get<SPF_GetManifestAPI_Func>("SPF_GetManifestAPI")(&manifest) && manifest.BuildManifest,
              "manifest export missing");
        SPF_Plugin_Exports exports{};
        check(panel.get<decltype(&SPF_GetPlugin)>("SPF_GetPlugin")(&exports), "plugin export failed");
        auto ui = mock_ui();
        auto keys = mock_keys();
        SPF_Core_API api{};
        api.ui = &ui;
        api.keybinds = &keys;
        SPF_Load_API load{};
        exports.OnLoad(&load);
        exports.OnActivated(&api);
        exports.OnRegisterUI(&ui);
        check(spf.toggle && !spf.visible, "window started visible");
        spf.toggle();
        exports.OnUpdate();
        check(spf.focused, "opened without focus");
        rendered(ui, "CoachAnnouncer.dll is not loaded");

        spf.click_change = true;
        rendered(ui, "Left Ctrl + Left Shift + F8");
        check(spf.rebind_called, "did not request SPF key capture");
        spf.click_change = false;

        {
            Module core(argv[1]);
            const auto init = core.get<decltype(&scs_telemetry_init)>("scs_telemetry_init");
            const auto shutdown = core.get<decltype(&scs_telemetry_shutdown)>("scs_telemetry_shutdown");
            const auto read = core.get<coach::panel::Reader>(coach::panel::export_name);
            const auto send = core.get<coach::panel::Sender>(coach::panel::command_export);
            Snapshot snapshot;
            check(read(&snapshot, sizeof(snapshot) - 1) == Read::bad_size, "reject incompatible buffer");
            rendered(ui, "Waiting for the SCS telemetry session");
            scs_telemetry_init_params_v101_t params{};
            params.common = {"Euro Truck Simulator 2", "eut2", SCS_MAKE_VERSION(1, 21), 0, nullptr};
            params.register_for_event = register_event;
            params.unregister_from_event = unregister_event;
            check(init(SCS_TELEMETRY_VERSION_1_01, &params) == SCS_RESULT_ok, "SCS init failed");

            set_stop(0);
            rendered(ui, "Next stop\n塔林");
            check(spf.rendered.find("Terminal\n派尔努") != std::string::npos
               && spf.rendered.find("赫尔辛基") != std::string::npos
               && spf.rendered.find("Company service") != std::string::npos && spf.dots >= 3,
               "actual route data missing");
            spf.choose_stop = true;
            rendered(ui, "Manual target: 派尔努");
            spf.change_auto = true;
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            do {
                spf.draw(&ui, nullptr);
                if (read(&snapshot, sizeof(snapshot)) == Read::ready && !snapshot.audio.auto_next) break;
                std::this_thread::sleep_for(20ms);
            } while (std::chrono::steady_clock::now() < deadline);
            check(!snapshot.audio.auto_next, "checkbox did not update worker settings");
            const auto directory = std::filesystem::path(std::getenv("COACH_PROBE_TEST_DIR")).parent_path();
            std::ifstream saved(directory / "settings.ini");
            std::string ini((std::istreambuf_iterator<char>(saved)), {});
            check(ini.find("auto_next=off") != std::string::npos, "checkbox settings were not persisted");
            const auto route_revision = snapshot.route_revision;
            rendered(ui, "Audio engine unavailable");
            set_stop(1);
            rendered(ui, "Next stop\n派尔努");
            check(spf.rendered.find("Manual target:") == std::string::npos, "override survived leg change");
            Command stale{snapshot.session, 9000, route_revision, Operation::clear_override};
            const auto invalid = send(&stale, sizeof(stale)-1);
            check(invalid == Result::invalid, "malformed command size accepted");
            auto status = send(&stale, sizeof(stale));
            for (unsigned i = 0; status == Result::busy && i < 25; ++i) {
                std::this_thread::sleep_for(20ms);
                status = send(&stale, sizeof(stale));
            }
            check(status == Result::accepted, "command could not reach worker");
            const auto stale_deadline = std::chrono::steady_clock::now() + 2s;
            while (std::chrono::steady_clock::now() < stale_deadline) {
                if (read(&snapshot, sizeof(snapshot)) == Read::ready && snapshot.ticket == 9000) break;
                std::this_thread::sleep_for(20ms);
            }
            check(snapshot.ticket == 9000 && snapshot.command_result == Result::stale, "old route command executed");
            const scs_named_value_t empty[]{ {} };
            const scs_telemetry_gameplay_event_t cancel{"bus_job.cancelled", empty};
            sdk.events[SCS_TELEMETRY_EVENT_gameplay](SCS_TELEMETRY_EVENT_gameplay, &cancel, sdk.context);
            rendered(ui, "No active passenger contract");
            check(spf.rendered.find("塔林") == std::string::npos, "stale route");
            shutdown();
            check(read(&snapshot, sizeof(snapshot)) == Read::inactive, "session still reported live");
            rendered(ui, "Waiting for the SCS telemetry session");
        }
        rendered(ui, "CoachAnnouncer.dll is not loaded");

        spf.toggle();
        exports.OnUpdate();
        check(!spf.visible, "shortcut did not hide the window");
        exports.OnUnload();
        std::cout << "PASS: SPF lifecycle, graphics calls, interactive route selection, persisted settings, stale commands and core unload.\n";
    } catch (const std::exception& err) {
        std::cerr << err.what() << '\n';
        return 1;
    }
}
