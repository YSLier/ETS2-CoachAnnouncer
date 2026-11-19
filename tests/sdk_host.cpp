#include <scssdk_telemetry.h>

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Event {
    scs_telemetry_event_callback_t call{};
    scs_context_t context{};
};

std::array<Event, 7> events;
scs_telemetry_channel_callback_t time_call{};
scs_context_t time_context{};
scs_telemetry_channel_callback_t position_call{};
scs_context_t position_context{};
bool fail_gameplay{};
bool fail_time{};
struct Channel {
    const char* name;
    scs_telemetry_channel_callback_t call{};
    scs_context_t context{};
};
std::array<Channel, 4> navigation{{{"truck.speed"}, {"truck.navigation.distance"},
                                  {"truck.navigation.time"}, {"local.scale"}}};

SCSAPI_VOID log(scs_log_type_t level, scs_string_t message) {
    std::cout << "SDK [" << level << "] " << message << '\n';
}

SCSAPI_RESULT register_event(scs_event_t event, scs_telemetry_event_callback_t callback,
                             scs_context_t context) {
    if (event == SCS_TELEMETRY_EVENT_gameplay && fail_gameplay) {
        return SCS_RESULT_not_now;
    }
    if (events.at(event).call) {
        return SCS_RESULT_already_registered;
    }
    events.at(event) = {callback, context};
    return SCS_RESULT_ok;
}

SCSAPI_RESULT unregister_event(scs_event_t event) {
    events.at(event) = {};
    return SCS_RESULT_ok;
}

SCSAPI_RESULT register_channel(scs_string_t name, scs_u32_t index, scs_value_type_t type,
                               scs_u32_t flags, scs_telemetry_channel_callback_t callback,
                               scs_context_t context) {
    require(index == SCS_U32_NIL && (flags & SCS_TELEMETRY_CHANNEL_FLAG_no_value),
            "unexpected channel flags");
    if (std::strcmp(name, "game.time") == 0) {
        require(type == SCS_VALUE_TYPE_u32, "expected integer game.time");
        if (fail_time) {
            return SCS_RESULT_not_found;
        }
        time_call = callback;
        time_context = context;
        return SCS_RESULT_ok;
    }
    if (std::strcmp(name, "truck.world.placement") == 0) {
        require(type == SCS_VALUE_TYPE_dplacement, "expected double world placement");
        position_call = callback;
        position_context = context;
        return SCS_RESULT_ok;
    }
    for (auto& field : navigation) {
        if (std::strcmp(name, field.name) == 0) {
            require(type == SCS_VALUE_TYPE_float, "expected float navigation channel");
            field.call = callback;
            field.context = context;
            return SCS_RESULT_ok;
        }
    }
    return SCS_RESULT_not_found;
}

SCSAPI_RESULT unregister_channel(scs_string_t name, scs_u32_t, scs_value_type_t) {
    if (!name || std::strcmp(name, "truck.world.placement") == 0) {
        position_call = nullptr;
        position_context = nullptr;
    }
    if (!name || std::strcmp(name, "game.time") == 0) {
        time_call = nullptr;
        time_context = nullptr;
    }
    for (auto& field : navigation) {
        if (!name || std::strcmp(name, field.name) == 0) {
            field.call = nullptr;
            field.context = nullptr;
        }
    }
    return SCS_RESULT_ok;
}

void dispatch(scs_event_t id, const void* data = nullptr) {
    const auto event = events.at(id);
    require(event.call, "missing event callback");
    event.call(id, data, event.context);
}

scs_named_value_t field(const char* name, scs_u32_t index, scs_value_type_t type) {
    scs_named_value_t result{};
    result.name = name;
    result.index = index;
    result.value.type = type;
    return result;
}

scs_named_value_t string_field(const char* name, const char* value, scs_u32_t index = SCS_U32_NIL) {
    auto result = field(name, index, SCS_VALUE_TYPE_string);
    result.value.value_string.value = value;
    return result;
}

scs_named_value_t u32_field(const char* name, scs_u32_t value, scs_u32_t index = SCS_U32_NIL) {
    auto result = field(name, index, SCS_VALUE_TYPE_u32);
    result.value.value_u32.value = value;
    return result;
}

scs_named_value_t s64_field(const char* name, scs_s64_t value) {
    auto result = field(name, SCS_U32_NIL, SCS_VALUE_TYPE_s64);
    result.value.value_s64.value = value;
    return result;
}

void config(const char* id, const scs_named_value_t* attributes) {
    const scs_telemetry_configuration_t payload{id, attributes};
    dispatch(SCS_TELEMETRY_EVENT_configuration, &payload);
}

void gameplay(const char* id, const scs_named_value_t* attributes) {
    const scs_telemetry_gameplay_event_t payload{id, attributes};
    dispatch(SCS_TELEMETRY_EVENT_gameplay, &payload);
}

void tick(scs_u32_t flags, scs_u32_t minutes) {
    const scs_telemetry_frame_start_t frame{flags, 0, minutes * 1000ULL,
                                           minutes * 1000ULL, minutes * 1000ULL};
    dispatch(SCS_TELEMETRY_EVENT_frame_start, &frame);
    if (time_call) {
        const auto time = u32_field("game.time", minutes).value;
        time_call("game.time", SCS_U32_NIL, &time, time_context);
    }
}

void simulated_trips() {
    constexpr scs_named_value_t empty[]{ {} };
    config("bus_job", empty);
    config("truck", empty);
    dispatch(SCS_TELEMETRY_EVENT_started);
    tick(SCS_TELEMETRY_FRAME_START_FLAG_timer_restart, 100);
    char terminus[] = "München \"ZOB\"";
    auto non_finite = field("future.non_finite", SCS_U32_NIL, SCS_VALUE_TYPE_double);
    non_finite.value.value_double.value = std::numeric_limits<double>::infinity();
    auto vector = field("destination_stop.future_vector", 1, SCS_VALUE_TYPE_dvector);
    vector.value.value_dvector = {1.25, 0.0, -3.5};
    scs_named_value_t company[]{
        string_field("bus_job.market", "company_job"),
        u32_field("bus.stages.count", 2),
        u32_field("bus.stages.current", 0),
        string_field("destination_stop.name", terminus, 1),
        string_field("source_stop.type", "city", 0),
        string_field("source_stop.id", "nurnberg", 0),
        string_field("source_stop.name", "Nürnberg", 0),
        string_field("destination_stop.id", "ingolstadt", 0),
        string_field("destination_stop.name", "Ingolstadt", 0),
        string_field("source_stop.name", "Ingolstadt", 1),
        u32_field("destination_stop.arrival_time.planned", 0, 0),
        u32_field("source_stop.entering_passengers.planned", 12, 0),
        u32_field("destination_stop.leaving_passengers.actual", 8, 1),
        field("future.value", 470, 81),
        vector,
        non_finite,
        u32_field("source_stop.departure_time.actual", 101, 0),
        {},
        {}
    };
    config("bus_job", company);
    config("job", empty);
    config("bus_job", company);
    require(position_call, "world placement was not registered");
    scs_value_t position{};
    position.type = SCS_VALUE_TYPE_dplacement;
    position.value_dplacement.position = {-4021, 0, 9431};
    position_call("truck.world.placement", SCS_U32_NIL, &position, position_context);
    company[17] = u32_field("destination_stop.arrival_time.actual", 109, 0);
    company[2] = u32_field("bus.stages.current", 1);
    config("bus_job", company);
    tick(0, 1000);
    constexpr std::array<float, navigation.size()> values{12, 2100, 120, 19};
    for (std::size_t i = 0; i < navigation.size(); ++i) {
        const auto& channel = navigation[i];
        require(channel.call, "missing navigation callback");
        scs_value_t value{};
        value.type = SCS_VALUE_TYPE_float;
        value.value_float.value = values[i];
        channel.call(channel.name, SCS_U32_NIL, &value, channel.context);
    }
    dispatch(SCS_TELEMETRY_EVENT_frame_end);
    terminus[0] = 'X'; // Game-owned strings can be reused immediately after the callback.
    dispatch(SCS_TELEMETRY_EVENT_paused);
    config("bus_job", empty); // Exercise an empty configuration before the completion event.
    const scs_named_value_t payout[]{s64_field("revenue", 9007199254741001LL),
        u32_field("passenger.satisfaction", 90), {}};
    gameplay("bus_job.completed", payout);

    using namespace std::chrono_literals;
    std::this_thread::sleep_for(200ms);
    dispatch(SCS_TELEMETRY_EVENT_started);
    tick(0, 160);
    scs_named_value_t tourist[]{
        string_field("bus_job.market", "tourist_job"),
        u32_field("bus.stages.count", 2),
        u32_field("bus.stages.current", 0),
        string_field("source_stop.name", "Oulu", 0),
        string_field("destination_stop.id", "album.viewpoints.viitasaari", 0),
        string_field("destination_stop.type", "viewpoints", 0),
        string_field("destination_stop.name", "Viitasaari", 0),
        string_field("source_stop.name", "Viitasaari", 1),
        string_field("destination_stop.name", "Oulu", 1),
        u32_field("destination_stop.arrival_time.planned", 300, 1),
        {}
    };
    config("bus_job", tourist);
    tick(0, 240);
    tourist[2] = u32_field("bus.stages.current", 1);
    config("bus_job", tourist);
    tick(0, 300);
    tourist[9] = u32_field("destination_stop.arrival_time.actual", 300, 1);
    config("bus_job", tourist);
    gameplay("bus_job.completed", payout);
    config("bus_job", empty);
    config("bus_job", tourist);
    const scs_named_value_t penalty[]{s64_field("penalty", -500), {}};
    gameplay("bus_job.cancelled", penalty);
    config("bus_job", empty);
    gameplay("bus_job.future_event", empty);
    config("job", penalty);
    config("bus_job", nullptr);
}

void limits_and_reload() {
    const std::string oversized = std::string(4095, 'a') + "ü";
    const scs_named_value_t large_route[]{
        s64_field("bus.stages.count", 9999999),
        s64_field("bus.stages.current", -1),
        string_field("source_stop.name", oversized.c_str(), 127),
        string_field("destination_stop.name", "out of bounds", 128),
        string_field("destination_stop.name", "unindexed"),
        u32_field("destination_stop.arrival_time.actual", 700, 0),
        {}
    };
    config("bus_job", large_route);
    const scs_named_value_t malformed[]{u32_field("bus.stages.count", 1),
        u32_field("bus.stages.current", 4), string_field("destination_stop.name", "X", 4), {}};
    config("bus_job", malformed);
    const scs_named_value_t changed[]{string_field("bus_job.market", "tourist_job"), {}};
    config("bus_job", changed);
    tick(SCS_TELEMETRY_FRAME_START_FLAG_timer_restart, 100);
    config("bus_job", changed);
    std::vector<scs_named_value_t> many(4100, u32_field("future.field", 1));
    many.push_back({});
    config("bus_job", many.data());
    std::vector<scs_named_value_t> strings(80, string_field("future.string", oversized.c_str()));
    strings.push_back({});
    config("bus_job", strings.data());
}

void burst() {
    const scs_named_value_t cheap[]{u32_field("bus.stages.count", 1), {}};
    for (unsigned i = 0; i < 5000; ++i) {
        config("bus_job", cheap);
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        require(argc == 2, "expected DLL path");
#ifdef _WIN32
        const auto module = LoadLibraryW(argv[1]);
        require(module, "LoadLibraryW failed");
        std::cout << "PID=" << GetCurrentProcessId() << '\n';
        const auto init = reinterpret_cast<decltype(&scs_telemetry_init)>(
            GetProcAddress(module, "scs_telemetry_init"));
        const auto shutdown = reinterpret_cast<decltype(&scs_telemetry_shutdown)>(
            GetProcAddress(module, "scs_telemetry_shutdown"));
#else
        const auto module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        require(module, "dlopen failed");
        std::cout << "PID=" << getpid() << '\n';
        const auto init = reinterpret_cast<decltype(&scs_telemetry_init)>(
            dlsym(module, "scs_telemetry_init"));
        const auto shutdown = reinterpret_cast<decltype(&scs_telemetry_shutdown)>(
            dlsym(module, "scs_telemetry_shutdown"));
#endif
        require(init && shutdown, "SDK exports missing");
        scs_telemetry_init_params_v101_t params{};
        params.common = {"Euro Truck Simulator 2 1.62.3.3s", "eut2", SCS_MAKE_VERSION(1, 21), 0, log};
        params.register_for_event = register_event;
        params.unregister_from_event = unregister_event;
        params.register_for_channel = register_channel;
        params.unregister_from_channel = unregister_channel;

        require(init(SCS_TELEMETRY_VERSION_1_00, &params) == SCS_RESULT_unsupported,
                "wrong API version should be rejected");
        params.common.game_id = "ats";
        require(init(SCS_TELEMETRY_VERSION_1_01, &params) == SCS_RESULT_unsupported,
                "wrong game should be rejected");
        params.common.game_id = "eut2";
        fail_gameplay = true;
        require(init(SCS_TELEMETRY_VERSION_1_01, &params) == SCS_RESULT_generic_error,
                "required registration failure should fail init");
        require(!events[SCS_TELEMETRY_EVENT_configuration].call,
                "failed init should roll back configuration registration");
        fail_gameplay = false;

        for (auto scenario : {simulated_trips, limits_and_reload, burst}) {
            fail_time = scenario == limits_and_reload;
            require(init(SCS_TELEMETRY_VERSION_1_01, &params) == SCS_RESULT_ok,
                    "SDK initialization failed");
            scenario();
            shutdown();
            // SDK guarantees cleanup of any remaining registrations after shutdown returns.
            events.fill({});
            unregister_channel(nullptr, 0, 0);
        }
        shutdown();
#ifdef _WIN32
        require(FreeLibrary(module), "FreeLibrary failed");
#else
        require(dlclose(module) == 0, "dlclose failed");
#endif
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
