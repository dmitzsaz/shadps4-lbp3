// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/pad/pad_errors.h"
#include "core/user_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "pad.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>

namespace Libraries::Pad {

using Input::GameController;
using Input::GameControllers;
using namespace Libraries::UserService;

struct HandleKey {
    OrbisUserServiceUserId id;
    s32 device_class;
    s32 index;

    bool operator==(const HandleKey&) const = default;
};
struct HandleKeyHash {
    std::size_t operator()(const HandleKey& k) const {
        std::size_t h1 = std::hash<OrbisUserServiceUserId>{}(k.id);
        std::size_t h2 = std::hash<s32>{}(k.device_class);
        std::size_t h3 = std::hash<s32>{}(k.index);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

static bool g_initialized = false;
static u64 pad_handle_counter = 1;
static std::unordered_map<HandleKey, s32, HandleKeyHash> pad_handle_map{};
static std::unordered_map<s32, GameController*> handle_to_controller_map{};

namespace {

enum class InputTraceMode : u32 {
    Disabled,
    Record,
    Replay,
};

enum class InputTraceCallKind : u32 {
    Read = 1,
    ReadState = 2,
};

constexpr std::array<char, 8> InputTraceMagic{'S', '4', 'P', 'A', 'D', 'T', 'R', '1'};
constexpr u32 InputTraceVersion = 1;

struct InputTraceHeader {
    std::array<char, 8> magic{};
    u32 version{};
    u32 pad_data_size{};
};

struct InputTraceCall {
    u32 kind{};
    s32 handle{};
    s32 requested_count{};
    s32 return_value{};
    u32 data_count{};
    u32 reserved{};
    u64 sequence{};
    u64 process_time_us{};
};

void FlushInputTraceAtQuickExit();

class InputTrace {
public:
    bool Configure(const std::filesystem::path& record_path,
                   const std::filesystem::path& replay_path) {
        std::scoped_lock lock{mutex};
        if (!record_path.empty() && !replay_path.empty()) {
            LOG_CRITICAL(Lib_Pad, "Only one of --pad-record and --pad-replay may be used");
            return false;
        }

        mode = InputTraceMode::Disabled;
        sequence = 0;
        replay_failed = false;
        record.close();
        replay.close();

        if (record_path.empty() && replay_path.empty()) {
            return true;
        }

        if (!record_path.empty()) {
            std::error_code ec;
            const auto parent = record_path.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
            }
            if (ec) {
                LOG_CRITICAL(Lib_Pad, "Failed to create pad-trace directory {}: {}",
                             parent.string(), ec.message());
                return false;
            }
            record.open(record_path, std::ios::binary | std::ios::trunc);
            if (!record) {
                LOG_CRITICAL(Lib_Pad, "Failed to open pad trace for recording: {}",
                             record_path.string());
                return false;
            }
            const InputTraceHeader header{
                .magic = InputTraceMagic,
                .version = InputTraceVersion,
                .pad_data_size = sizeof(OrbisPadData),
            };
            record.write(reinterpret_cast<const char*>(&header), sizeof(header));
            record.flush();
            if (!record) {
                LOG_CRITICAL(Lib_Pad, "Failed to write pad-trace header: {}",
                             record_path.string());
                return false;
            }
            mode = InputTraceMode::Record;
            LOG_CRITICAL(Lib_Pad, "Recording guest pad calls to {}", record_path.string());
        } else {
            replay.open(replay_path, std::ios::binary);
            InputTraceHeader header{};
            replay.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (!replay || header.magic != InputTraceMagic || header.version != InputTraceVersion ||
                header.pad_data_size != sizeof(OrbisPadData)) {
                LOG_CRITICAL(Lib_Pad, "Invalid or incompatible pad trace: {}",
                             replay_path.string());
                replay.close();
                return false;
            }
            mode = InputTraceMode::Replay;
            LOG_CRITICAL(Lib_Pad, "Replaying guest pad calls from {}", replay_path.string());
        }

        static const bool registered = [] {
            std::at_quick_exit(FlushInputTraceAtQuickExit);
            return true;
        }();
        (void)registered;
        return true;
    }

    void Record(InputTraceCallKind kind, s32 handle, s32 requested_count, s32 return_value,
                const OrbisPadData* data, u32 data_count) {
        std::scoped_lock lock{mutex};
        if (mode != InputTraceMode::Record) {
            return;
        }
        const InputTraceCall call{
            .kind = static_cast<u32>(kind),
            .handle = handle,
            .requested_count = requested_count,
            .return_value = return_value,
            .data_count = data_count,
            .sequence = ++sequence,
            .process_time_us = Kernel::sceKernelGetProcessTime(),
        };
        record.write(reinterpret_cast<const char*>(&call), sizeof(call));
        if (data_count != 0) {
            record.write(reinterpret_cast<const char*>(data), sizeof(OrbisPadData) * data_count);
        }
        if ((sequence & 0xff) == 0) {
            record.flush();
        }
        if (!record) {
            LOG_CRITICAL(Lib_Pad, "Pad trace write failed at call {}", sequence);
            mode = InputTraceMode::Disabled;
        }
    }

    std::optional<s32> Replay(InputTraceCallKind kind, s32 handle, s32 requested_count,
                              OrbisPadData* data) {
        std::scoped_lock lock{mutex};
        if (mode != InputTraceMode::Replay) {
            return std::nullopt;
        }

        InputTraceCall call{};
        replay.read(reinterpret_cast<char*>(&call), sizeof(call));
        if (!replay) {
            if (!replay_failed) {
                LOG_CRITICAL(Lib_Pad, "Pad trace ended after {} calls; returning to live input",
                             sequence);
            }
            replay_failed = true;
            mode = InputTraceMode::Disabled;
            return std::nullopt;
        }

        std::vector<OrbisPadData> samples(call.data_count);
        if (call.data_count != 0) {
            replay.read(reinterpret_cast<char*>(samples.data()),
                        sizeof(OrbisPadData) * call.data_count);
        }
        const u64 expected_sequence = sequence + 1;
        const bool valid = replay && call.sequence == expected_sequence &&
                           call.kind == static_cast<u32>(kind) && call.handle == handle &&
                           call.requested_count == requested_count && call.data_count <=
                               static_cast<u32>(std::max(requested_count, 0));
        if (!valid) {
            LOG_CRITICAL(Lib_Pad,
                         "Pad trace mismatch at call {}: got kind={} handle={} requested={} "
                         "samples={}, expected kind={} handle={} requested={}; returning to live "
                         "input",
                         expected_sequence, call.kind, call.handle, call.requested_count,
                         call.data_count, static_cast<u32>(kind), handle, requested_count);
            replay_failed = true;
            mode = InputTraceMode::Disabled;
            return std::nullopt;
        }

        const u64 now = Kernel::sceKernelGetProcessTime();
        for (auto& sample : samples) {
            const u64 age = call.process_time_us >= sample.timestamp
                                ? call.process_time_us - sample.timestamp
                                : 0;
            sample.timestamp = now >= age ? now - age : 0;
        }
        if (!samples.empty()) {
            std::memcpy(data, samples.data(), sizeof(OrbisPadData) * samples.size());
        }
        sequence = call.sequence;
        if (sequence <= 4 || std::has_single_bit(sequence)) {
            LOG_INFO(Lib_Pad, "Replayed pad call #{} kind={} samples={}", sequence, call.kind,
                     call.data_count);
        }
        return call.return_value;
    }

    void Flush() {
        std::scoped_lock lock{mutex};
        if (record.is_open()) {
            record.flush();
        }
    }

private:
    InputTraceMode mode{InputTraceMode::Disabled};
    u64 sequence{};
    bool replay_failed{};
    std::ofstream record;
    std::ifstream replay;
    std::mutex mutex;
};

InputTrace input_trace;

void FlushInputTraceAtQuickExit() {
    input_trace.Flush();
}

} // namespace

bool ConfigureInputTrace(const std::filesystem::path& record_path,
                         const std::filesystem::path& replay_path) {
    return input_trace.Configure(record_path, replay_path);
}

int PS4_SYSV_ABI scePadClose(s32 handle) {
    LOG_WARNING(Lib_Pad, "called, handle: {}", handle);
    if (handle_to_controller_map.erase(handle) == 0) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    for (auto& it : pad_handle_map) {
        if (it.second == handle) {
            pad_handle_map.erase(it.first);
            break;
        }
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadConnectPort() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDeviceClassGetExtendedInformation(
    s32 handle, OrbisPadDeviceClassExtendedInformation* pExtInfo) {
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    std::memset(pExtInfo, 0, sizeof(OrbisPadDeviceClassExtendedInformation));
    if (EmulatorSettings.IsUsingSpecialPad()) {
        pExtInfo->deviceClass = (OrbisPadDeviceClass)EmulatorSettings.GetSpecialPadClass();
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDeviceClassParseData(s32 handle, const OrbisPadData* pData,
                                            OrbisPadDeviceClassData* pDeviceClassData) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDeviceOpen() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDisableVibration() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDisconnectDevice() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDisconnectPort() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadEnableAutoDetect() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadEnableExtensionPort() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadEnableSpecificDeviceClass() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadEnableUsbConnection() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetBluetoothAddress() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetCapability() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetControllerInformation(s32 handle, OrbisPadControllerInformation* pInfo) {
    LOG_DEBUG(Lib_Pad, "called handle = {}", handle);
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    bool connected = false;
    int connected_count = 0;
    Input::State state{};
    it->second->ReadState(&state, &connected, &connected_count);

    std::memset(pInfo, 0, sizeof(OrbisPadControllerInformation));
    pInfo->touchPadInfo.pixelDensity = 1;
    pInfo->touchPadInfo.resolution.x = 1920;
    pInfo->touchPadInfo.resolution.y = 950;
    pInfo->stickInfo.deadZoneLeft = 1;
    pInfo->stickInfo.deadZoneRight = 1;
    pInfo->connectionType = ORBIS_PAD_CONNECTION_TYPE_LOCAL;
    pInfo->connectedCount = static_cast<u8>(std::clamp(connected_count, 0, 0xff));
    pInfo->deviceClass = OrbisPadDeviceClass::Standard;
    pInfo->connected = connected;
    if (connected) {
        pInfo->deviceClass = EmulatorSettings.IsUsingSpecialPad()
                                 ? (OrbisPadDeviceClass)EmulatorSettings.GetSpecialPadClass()
                                 : OrbisPadDeviceClass::Standard;
    }
    LOG_DEBUG(Lib_Pad, "c: {} cc: {}, ct: {}, dc: {}", pInfo->connected, pInfo->connectedCount,
              pInfo->connectionType, std::to_underlying(pInfo->deviceClass));
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetDataInternal() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetDeviceId() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetDeviceInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetExtControllerInformation(s32 handle,
                                                   OrbisPadExtendedControllerInformation* pInfo) {
    LOG_INFO(Lib_Pad, "called handle = {}", handle);
    std::memset(pInfo, 0, sizeof(OrbisPadExtendedControllerInformation));
    return scePadGetControllerInformation(handle, &pInfo->base);
}

int PS4_SYSV_ABI scePadGetExtensionUnitInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetFeatureReport() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetHandle(Libraries::UserService::OrbisUserServiceUserId userId, s32 type,
                                 s32 index) {
    if (!g_initialized) {
        return ORBIS_PAD_ERROR_NOT_INITIALIZED;
    }
    if (userId == -1) {
        return ORBIS_PAD_ERROR_DEVICE_NO_HANDLE;
    }
    auto it = pad_handle_map.find({userId, type, index});
    if (it == pad_handle_map.end()) {
        return ORBIS_PAD_ERROR_DEVICE_NO_HANDLE;
    }
    s32 pad_handle = it->second;
    LOG_DEBUG(Lib_Pad, "called, userid: {}, out pad handle: {}", userId, pad_handle);
    return pad_handle;
}

int PS4_SYSV_ABI scePadGetIdleCount() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetInfo(OrbisPadInfo* data) {
    LOG_WARNING(Lib_Pad, "(DUMMY) called");
    if (!data) {
        return ORBIS_PAD_ERROR_INVALID_ARG;
    }
    auto& controllers = *Common::Singleton<GameControllers>::Instance();
    auto col = controllers[0]->GetLightBarRGB();
    std::memset(data, 0, sizeof(OrbisPadInfo));
    data->unk1 = 0x1;
    data->pad_handle = 1;
    data->unk3 = 0x00000101;
    data->colour = col.r + (col.g << 8) + (col.b << 16);
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetInfoByPortType() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetLicenseControllerInformation() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetMotionSensorPosition() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetMotionTimerUnit() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetSphereRadius() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetVersionInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadInit() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    g_initialized = true;
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsBlasterConnected() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsDS4Connected() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsLightBarBaseBrightnessControllable() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsMoveConnected() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsMoveReproductionModel() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsValidHandle() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadMbusInit() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadMbusTerm() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadOpen(Libraries::UserService::OrbisUserServiceUserId userId, s32 type,
                            s32 index, const OrbisPadOpenParam* pParam) {
    if (!g_initialized) {
        return ORBIS_PAD_ERROR_NOT_INITIALIZED;
    }
    if (userId < 0) {
        return ORBIS_DEVICE_SERVICE_ERROR_INVALID_USER;
    }
    if (pad_handle_map.find({userId, type, index}) != pad_handle_map.end()) {
        return ORBIS_PAD_ERROR_ALREADY_OPENED;
    }
    auto& controllers = *Common::Singleton<GameControllers>::Instance();
    if (userId == ORBIS_USER_SERVICE_USER_ID_SYSTEM) {
        if (type == ORBIS_PAD_PORT_TYPE_REMOTE_CONTROL) {
            s32 new_handle = pad_handle_counter++;
            pad_handle_map[{userId, type, index}] = new_handle;
            handle_to_controller_map[new_handle] = controllers[4];
            LOG_INFO(Lib_Pad, "Opened a TV remote device, out handle: {}", new_handle);
            return new_handle;
        }
        return ORBIS_DEVICE_SERVICE_ERROR_INVALID_USER;
    }
    if (type == ORBIS_PAD_PORT_TYPE_REMOTE_CONTROL) {
        return ORBIS_PAD_ERROR_INVALID_ARG;
    }
    auto u = UserManagement.GetUserByID(userId);
    if (!u) {
        return ORBIS_DEVICE_SERVICE_ERROR_USER_NOT_LOGIN;
    }
    s32 new_handle = pad_handle_counter++;
    pad_handle_map[{userId, type, index}] = new_handle;

    handle_to_controller_map[new_handle] =
        controllers[type == (EmulatorSettings.IsUsingSpecialPad() ? 2 : 0)
                        ? UserManagement.GetUserByID(userId)->player_index - 1
                        : 4];
    LOG_INFO(Lib_Pad,
             "called user_id = {}, type = {}, index = {}, player index = {}, out handle = {}",
             userId, type, index, u->player_index, new_handle);
    scePadResetLightBar(new_handle);
    scePadResetOrientation(new_handle);
    return new_handle;
}

int PS4_SYSV_ABI scePadOpenExt(Libraries::UserService::OrbisUserServiceUserId userId, s32 type,
                               s32 index, const OrbisPadOpenExtParam* pParam) {
    LOG_WARNING(Lib_Pad, "Redirect to scePadOpen");
    return scePadOpen(userId, type, index, nullptr);
}

int PS4_SYSV_ABI scePadOpenExt2() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadOutputReport() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int ProcessStates(s32 handle, OrbisPadData* pData, Input::GameController& controller,
                  Input::State* states, s32 num, bool connected, u32 connected_count) {
    if (!connected) {
        pData[0] = {};
        pData[0].orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        pData[0].connected = false;
        return 1;
    }

    const bool gamepad_input_intercepted = ImGui::Core::IsGamepadInputCaptured();
    for (int i = 0; i < num; i++) {
        if (gamepad_input_intercepted) {
            pData[i] = {};
            pData[i].buttons = OrbisPadButtonDataOffset::Intercepted;
            pData[i].leftStick = {128, 128};
            pData[i].rightStick = {128, 128};
            pData[i].orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            pData[i].connected = connected;
            pData[i].timestamp = states[i].time;
            pData[i].connectedCount = connected_count;
            pData[i].deviceUniqueDataLen = 0;
            continue;
        }

        pData[i].buttons = states[i].buttonsState;
        pData[i].leftStick.x = states[i].axes[static_cast<int>(Input::Axis::LeftX)];
        pData[i].leftStick.y = states[i].axes[static_cast<int>(Input::Axis::LeftY)];
        pData[i].rightStick.x = states[i].axes[static_cast<int>(Input::Axis::RightX)];
        pData[i].rightStick.y = states[i].axes[static_cast<int>(Input::Axis::RightY)];
        pData[i].analogButtons.l2 = states[i].axes[static_cast<int>(Input::Axis::TriggerLeft)];
        pData[i].analogButtons.r2 = states[i].axes[static_cast<int>(Input::Axis::TriggerRight)];
        pData[i].acceleration.x = states[i].acceleration.x * 0.098;
        pData[i].acceleration.y = states[i].acceleration.y * 0.098;
        pData[i].acceleration.z = states[i].acceleration.z * 0.098;
        pData[i].angularVelocity.x = states[i].angularVelocity.x;
        pData[i].angularVelocity.y = states[i].angularVelocity.y;
        pData[i].angularVelocity.z = states[i].angularVelocity.z;
        pData[i].orientation = {0.0f, 0.0f, 0.0f, 1.0f};

        const auto gyro_poll_rate = controller.accel_poll_rate;
        if (gyro_poll_rate != 0.0f) {
            auto now = std::chrono::steady_clock::now();
            float deltaTime = std::chrono::duration_cast<std::chrono::microseconds>(
                                  now - controller.GetLastUpdate())
                                  .count() /
                              1000000.0f;
            controller.SetLastUpdate(now);
            Libraries::Pad::OrbisFQuaternion lastOrientation = controller.GetLastOrientation();
            Libraries::Pad::OrbisFQuaternion outputOrientation = {0.0f, 0.0f, 0.0f, 1.0f};
            GameControllers::CalculateOrientation(pData->acceleration, pData->angularVelocity,
                                                  deltaTime, lastOrientation, outputOrientation);
            pData[i].orientation = outputOrientation;
            controller.SetLastOrientation(outputOrientation);
        }
        pData[i].touchData.touchNum =
            (states[i].touchpad[0].state ? 1 : 0) + (states[i].touchpad[1].state ? 1 : 0);

        if (handle == 1) {
            if (controller.GetTouchCount() >= 127) {
                controller.SetTouchCount(0);
            }

            if (controller.GetSecondaryTouchCount() >= 127) {
                controller.SetSecondaryTouchCount(0);
            }

            if (pData->touchData.touchNum == 1 && controller.GetPreviousTouchNum() == 0) {
                controller.SetTouchCount(controller.GetTouchCount() + 1);
                controller.SetSecondaryTouchCount(controller.GetTouchCount());
            } else if (pData->touchData.touchNum == 2 && controller.GetPreviousTouchNum() == 1) {
                controller.SetSecondaryTouchCount(controller.GetSecondaryTouchCount() + 1);
            } else if (pData->touchData.touchNum == 0 && controller.GetPreviousTouchNum() > 0) {
                if (controller.GetTouchCount() < controller.GetSecondaryTouchCount()) {
                    controller.SetTouchCount(controller.GetSecondaryTouchCount());
                } else {
                    if (controller.WasSecondaryTouchReset()) {
                        controller.SetTouchCount(controller.GetSecondaryTouchCount());
                        controller.UnsetSecondaryTouchResetBool();
                    }
                }
            }

            controller.SetPreviousTouchNum(pData->touchData.touchNum);

            if (pData->touchData.touchNum == 1) {
                states[i].touchpad[0].ID = controller.GetTouchCount();
                states[i].touchpad[1].ID = 0;
            } else if (pData->touchData.touchNum == 2) {
                states[i].touchpad[0].ID = controller.GetTouchCount();
                states[i].touchpad[1].ID = controller.GetSecondaryTouchCount();
            }
        } else {
            states[i].touchpad[0].ID = 1;
            states[i].touchpad[1].ID = 2;
        }

        if (!states[i].touchpad[0].state && states[i].touchpad[1].state) {
            pData[i].touchData.touch[0].x = states[i].touchpad[1].x;
            pData[i].touchData.touch[0].y = states[i].touchpad[1].y;
            pData[i].touchData.touch[0].id = states[i].touchpad[1].ID;
        } else {
            pData[i].touchData.touch[0].x = states[i].touchpad[0].x;
            pData[i].touchData.touch[0].y = states[i].touchpad[0].y;
            pData[i].touchData.touch[0].id = states[i].touchpad[0].ID;
            pData[i].touchData.touch[1].x = states[i].touchpad[1].x;
            pData[i].touchData.touch[1].y = states[i].touchpad[1].y;
            pData[i].touchData.touch[1].id = states[i].touchpad[1].ID;
        }
        if (Common::ElfInfo::Instance().FirmwareVer() > Common::ElfInfo::FW_350) {
            pData[i].touchData.time_since_touch_held_down =
                controller.last_touch_down_timestamp == 0
                    ? 0
                    : states[i].time - controller.last_touch_down_timestamp;
        }
        pData[i].connected = connected;
        pData[i].timestamp = states[i].time;
        pData[i].connectedCount = connected_count;
        pData[i].deviceUniqueDataLen = 0;
    }

    return num;
}

int PS4_SYSV_ABI scePadRead(s32 handle, OrbisPadData* pData, s32 num) {
    LOG_TRACE(Lib_Pad, "called");
    if (const auto replayed = input_trace.Replay(InputTraceCallKind::Read, handle, num, pData)) {
        return *replayed;
    }
    int connected_count = 0;
    bool connected = false;
    std::vector<Input::State> states(64);
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    int ret_num = controller.ReadStates(states.data(), num, &connected, &connected_count);
    const int result = ProcessStates(handle, pData, controller, states.data(), ret_num, connected,
                                     connected_count);
    input_trace.Record(InputTraceCallKind::Read, handle, num, result, pData,
                       result > 0 ? static_cast<u32>(result) : 0);
    return result;
}

int PS4_SYSV_ABI scePadReadBlasterForTracker() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadExt() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadForTracker() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadHistory() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadState(s32 handle, OrbisPadData* pData) {
    LOG_TRACE(Lib_Pad, "handle: {}", handle);
    if (const auto replayed =
            input_trace.Replay(InputTraceCallKind::ReadState, handle, 1, pData)) {
        return *replayed;
    }
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    int connected_count = 0;
    bool connected = false;
    Input::State state;
    controller.ReadState(&state, &connected, &connected_count);
    ProcessStates(handle, pData, controller, &state, 1, connected, connected_count);
    input_trace.Record(InputTraceCallKind::ReadState, handle, 1, ORBIS_OK, pData, 1);
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadStateExt() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetLightBar(s32 handle) {
    LOG_DEBUG(Lib_Pad, "called, handle: {}", handle);
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    auto u = UserManagement.GetUserByPlayerIndex(controller.user_id);
    s32 colour_index = u ? u->user_color - 1 : 0;
    Input::Colour colour{255, 0, 0};
    if (colour_index >= 0 && colour_index <= 3) {
        colour = Input::g_user_colours[colour_index];
    } else {
        LOG_ERROR(Lib_Pad, "Invalid user colour value {} for controller {}, falling back to blue",
                  colour_index, handle);
    }
    controller.SetLightBarRGB(colour);
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetLightBarAll() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetLightBarAllByPortType() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetOrientation(s32 handle) {
    LOG_INFO(Lib_Pad, "scePadResetOrientation called handle = {}", handle);

    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    Libraries::Pad::OrbisFQuaternion defaultOrientation = {0.0f, 0.0f, 0.0f, 1.0f};
    controller.SetLastOrientation(defaultOrientation);
    controller.SetLastUpdate(std::chrono::steady_clock::now());

    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetOrientationForTracker() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetAngularVelocityBiasCorrectionState() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetAngularVelocityDeadbandState(s32 handle, bool bEnable) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetAutoPowerOffCount() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetButtonRemappingInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetConnection() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetExtensionReport() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetFeatureReport() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetForceIntercepted() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetLightBar(s32 handle, const OrbisPadLightBarParam* pParam) {
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    if (pParam != nullptr) {
        LOG_DEBUG(Lib_Pad, "called handle = {} rgb = {} {} {}", handle, pParam->r, pParam->g,
                  pParam->b);

        if (pParam->r < 0xD && pParam->g < 0xD && pParam->b < 0xD) {
            LOG_INFO(Lib_Pad, "Invalid lightbar setting");
            return ORBIS_PAD_ERROR_INVALID_LIGHTBAR_SETTING;
        }

        auto& controllers = *Common::Singleton<GameControllers>::Instance();
        controller.SetLightBarRGB(pParam->r, pParam->g, pParam->b);
        return ORBIS_OK;
    }
    return ORBIS_PAD_ERROR_INVALID_ARG;
}

int PS4_SYSV_ABI scePadSetLightBarBaseBrightness() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetLightBarBlinking() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetLightBarForTracker(s32 handle, const OrbisPadLightBarParam* pParam) {
    LOG_INFO(Lib_Pad, "called, r: {} g: {} b: {}", pParam->r, pParam->g, pParam->b);
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    controller.SetLightBarRGB(pParam->r, pParam->g, pParam->b);
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetLoginUserNumber() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetMotionSensorState(s32 handle, bool bEnable) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
    // it's already handled by the SDL backend and will be on no matter what
    // (assuming the controller supports it)
}

int PS4_SYSV_ABI scePadSetProcessFocus() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetProcessPrivilege() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetProcessPrivilegeOfButtonRemapping() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetShareButtonMaskForRemotePlay() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetTiltCorrectionState(s32 handle, bool bEnable) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetUserColor() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetVibration(s32 handle, const OrbisPadVibrationParam* pParam) {
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    if (pParam != nullptr) {
        LOG_DEBUG(Lib_Pad, "scePadSetVibration called handle = {} data = {} , {}", handle,
                  pParam->smallMotor, pParam->largeMotor);
        controller.SetVibration(pParam->smallMotor, pParam->largeMotor);
        return ORBIS_OK;
    }
    return ORBIS_PAD_ERROR_INVALID_ARG;
}

int PS4_SYSV_ABI scePadSetVibrationForce() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetVrTrackingMode() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadShareOutputData() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadStartRecording() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadStopRecording() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSwitchConnection() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVertualDeviceAddDevice() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceAddDevice() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceDeleteDevice() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceDisableButtonRemapping() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceGetRemoteSetting() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceInsertData() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_298D21481F94C9FA() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_51E514BCD3A05CA5() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_89C9237E393DA243() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_EF103E845B6F0420() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("6ncge5+l5Qs", "libScePad", 1, "libScePad", scePadClose);
    LIB_FUNCTION("kazv1NzSB8c", "libScePad", 1, "libScePad", scePadConnectPort);
    LIB_FUNCTION("AcslpN1jHR8", "libScePad", 1, "libScePad",
                 scePadDeviceClassGetExtendedInformation);
    LIB_FUNCTION("IHPqcbc0zCA", "libScePad", 1, "libScePad", scePadDeviceClassParseData);
    LIB_FUNCTION("d7bXuEBycDI", "libScePad", 1, "libScePad", scePadDeviceOpen);
    LIB_FUNCTION("0aziJjRZxqQ", "libScePad", 1, "libScePad", scePadDisableVibration);
    LIB_FUNCTION("pnZXireDoeI", "libScePad", 1, "libScePad", scePadDisconnectDevice);
    LIB_FUNCTION("9ez71nWSvD0", "libScePad", 1, "libScePad", scePadDisconnectPort);
    LIB_FUNCTION("77ooWxGOIVs", "libScePad", 1, "libScePad", scePadEnableAutoDetect);
    LIB_FUNCTION("+cE4Jx431wc", "libScePad", 1, "libScePad", scePadEnableExtensionPort);
    LIB_FUNCTION("E1KEw5XMGQQ", "libScePad", 1, "libScePad", scePadEnableSpecificDeviceClass);
    LIB_FUNCTION("DD-KiRLBqkQ", "libScePad", 1, "libScePad", scePadEnableUsbConnection);
    LIB_FUNCTION("Q66U8FdrMaw", "libScePad", 1, "libScePad", scePadGetBluetoothAddress);
    LIB_FUNCTION("qtasqbvwgV4", "libScePad", 1, "libScePad", scePadGetCapability);
    LIB_FUNCTION("gjP9-KQzoUk", "libScePad", 1, "libScePad", scePadGetControllerInformation);
    LIB_FUNCTION("Uq6LgTJEmQs", "libScePad", 1, "libScePad", scePadGetDataInternal);
    LIB_FUNCTION("hDgisSGkOgw", "libScePad", 1, "libScePad", scePadGetDeviceId);
    LIB_FUNCTION("4rS5zG7RFaM", "libScePad", 1, "libScePad", scePadGetDeviceInfo);
    LIB_FUNCTION("hGbf2QTBmqc", "libScePad", 1, "libScePad", scePadGetExtControllerInformation);
    LIB_FUNCTION("1DmZjZAuzEM", "libScePad", 1, "libScePad", scePadGetExtensionUnitInfo);
    LIB_FUNCTION("PZSoY8j0Pko", "libScePad", 1, "libScePad", scePadGetFeatureReport);
    LIB_FUNCTION("u1GRHp+oWoY", "libScePad", 1, "libScePad", scePadGetHandle);
    LIB_FUNCTION("kiA9bZhbnAg", "libScePad", 1, "libScePad", scePadGetIdleCount);
    LIB_FUNCTION("1Odcw19nADw", "libScePad", 1, "libScePad", scePadGetInfo);
    LIB_FUNCTION("4x5Im8pr0-4", "libScePad", 1, "libScePad", scePadGetInfoByPortType);
    LIB_FUNCTION("vegw8qax5MI", "libScePad", 1, "libScePad", scePadGetLicenseControllerInformation);
    LIB_FUNCTION("WPIB7zBWxVE", "libScePad", 1, "libScePad", scePadGetMotionSensorPosition);
    LIB_FUNCTION("k4+nDV9vbT0", "libScePad", 1, "libScePad", scePadGetMotionTimerUnit);
    LIB_FUNCTION("do-JDWX+zRs", "libScePad", 1, "libScePad", scePadGetSphereRadius);
    LIB_FUNCTION("QuOaoOcSOw0", "libScePad", 1, "libScePad", scePadGetVersionInfo);
    LIB_FUNCTION("hv1luiJrqQM", "libScePad", 1, "libScePad", scePadInit);
    LIB_FUNCTION("bi0WNvZ1nug", "libScePad", 1, "libScePad", scePadIsBlasterConnected);
    LIB_FUNCTION("mEC+xJKyIjQ", "libScePad", 1, "libScePad", scePadIsDS4Connected);
    LIB_FUNCTION("d2Qk-i8wGak", "libScePad", 1, "libScePad",
                 scePadIsLightBarBaseBrightnessControllable);
    LIB_FUNCTION("4y9RNPSBsqg", "libScePad", 1, "libScePad", scePadIsMoveConnected);
    LIB_FUNCTION("9e56uLgk5y0", "libScePad", 1, "libScePad", scePadIsMoveReproductionModel);
    LIB_FUNCTION("pFTi-yOrVeQ", "libScePad", 1, "libScePad", scePadIsValidHandle);
    LIB_FUNCTION("CfwUlQtCFi4", "libScePad", 1, "libScePad", scePadMbusInit);
    LIB_FUNCTION("s7CvzS+9ZIs", "libScePad", 1, "libScePad", scePadMbusTerm);
    LIB_FUNCTION("xk0AcarP3V4", "libScePad", 1, "libScePad", scePadOpen);
    LIB_FUNCTION("WFIiSfXGUq8", "libScePad", 1, "libScePad", scePadOpenExt);
    LIB_FUNCTION("71E9e6n+2R8", "libScePad", 1, "libScePad", scePadOpenExt2);
    LIB_FUNCTION("DrUu8cPrje8", "libScePad", 1, "libScePad", scePadOutputReport);
    LIB_FUNCTION("q1cHNfGycLI", "libScePad", 1, "libScePad", scePadRead);
    LIB_FUNCTION("fm1r2vv5+OU", "libScePad", 1, "libScePad", scePadReadBlasterForTracker);
    LIB_FUNCTION("QjwkT2Ycmew", "libScePad", 1, "libScePad", scePadReadExt);
    LIB_FUNCTION("2NhkFTRnXHk", "libScePad", 1, "libScePad", scePadReadForTracker);
    LIB_FUNCTION("3u4M8ck9vJM", "libScePad", 1, "libScePad", scePadReadHistory);
    LIB_FUNCTION("YndgXqQVV7c", "libScePad", 1, "libScePad", scePadReadState);
    LIB_FUNCTION("5Wf4q349s+Q", "libScePad", 1, "libScePad", scePadReadStateExt);
    LIB_FUNCTION("DscD1i9HX1w", "libScePad", 1, "libScePad", scePadResetLightBar);
    LIB_FUNCTION("+4c9xRLmiXQ", "libScePad", 1, "libScePad", scePadResetLightBarAll);
    LIB_FUNCTION("+Yp6+orqf1M", "libScePad", 1, "libScePad", scePadResetLightBarAllByPortType);
    LIB_FUNCTION("rIZnR6eSpvk", "libScePad", 1, "libScePad", scePadResetOrientation);
    LIB_FUNCTION("jbAqAvLEP4A", "libScePad", 1, "libScePad", scePadResetOrientationForTracker);
    LIB_FUNCTION("KLmYx9ij2h0", "libScePad", 1, "libScePad",
                 scePadSetAngularVelocityBiasCorrectionState);
    LIB_FUNCTION("r44mAxdSG+U", "libScePad", 1, "libScePad", scePadSetAngularVelocityDeadbandState);
    LIB_FUNCTION("ew647HuKi2Y", "libScePad", 1, "libScePad", scePadSetAutoPowerOffCount);
    LIB_FUNCTION("MbTt1EHYCTg", "libScePad", 1, "libScePad", scePadSetButtonRemappingInfo);
    LIB_FUNCTION("MLA06oNfF+4", "libScePad", 1, "libScePad", scePadSetConnection);
    LIB_FUNCTION("bsbHFI0bl5s", "libScePad", 1, "libScePad", scePadSetExtensionReport);
    LIB_FUNCTION("xqgVCEflEDY", "libScePad", 1, "libScePad", scePadSetFeatureReport);
    LIB_FUNCTION("lrjFx4xWnY8", "libScePad", 1, "libScePad", scePadSetForceIntercepted);
    LIB_FUNCTION("RR4novUEENY", "libScePad", 1, "libScePad", scePadSetLightBar);
    LIB_FUNCTION("dhQXEvmrVNQ", "libScePad", 1, "libScePad", scePadSetLightBarBaseBrightness);
    LIB_FUNCTION("etaQhgPHDRY", "libScePad", 1, "libScePad", scePadSetLightBarBlinking);
    LIB_FUNCTION("iHuOWdvQVpg", "libScePad", 1, "libScePad", scePadSetLightBarForTracker);
    LIB_FUNCTION("o-6Y99a8dKU", "libScePad", 1, "libScePad", scePadSetLoginUserNumber);
    LIB_FUNCTION("clVvL4ZDntw", "libScePad", 1, "libScePad", scePadSetMotionSensorState);
    LIB_FUNCTION("flYYxek1wy8", "libScePad", 1, "libScePad", scePadSetProcessFocus);
    LIB_FUNCTION("DmBx8K+jDWw", "libScePad", 1, "libScePad", scePadSetProcessPrivilege);
    LIB_FUNCTION("FbxEpTRDou8", "libScePad", 1, "libScePad",
                 scePadSetProcessPrivilegeOfButtonRemapping);
    LIB_FUNCTION("yah8Bk4TcYY", "libScePad", 1, "libScePad", scePadSetShareButtonMaskForRemotePlay);
    LIB_FUNCTION("vDLMoJLde8I", "libScePad", 1, "libScePad", scePadSetTiltCorrectionState);
    LIB_FUNCTION("z+GEemoTxOo", "libScePad", 1, "libScePad", scePadSetUserColor);
    LIB_FUNCTION("yFVnOdGxvZY", "libScePad", 1, "libScePad", scePadSetVibration);
    LIB_FUNCTION("8BOObG94-tc", "libScePad", 1, "libScePad", scePadSetVibrationForce);
    LIB_FUNCTION("--jrY4SHfm8", "libScePad", 1, "libScePad", scePadSetVrTrackingMode);
    LIB_FUNCTION("zFJ35q3RVnY", "libScePad", 1, "libScePad", scePadShareOutputData);
    LIB_FUNCTION("80XdmVYsNPA", "libScePad", 1, "libScePad", scePadStartRecording);
    LIB_FUNCTION("gAHvg6JPIic", "libScePad", 1, "libScePad", scePadStopRecording);
    LIB_FUNCTION("Oi7FzRWFr0Y", "libScePad", 1, "libScePad", scePadSwitchConnection);
    LIB_FUNCTION("0MB5x-ieRGI", "libScePad", 1, "libScePad", scePadVertualDeviceAddDevice);
    LIB_FUNCTION("N7tpsjWQ87s", "libScePad", 1, "libScePad", scePadVirtualDeviceAddDevice);
    LIB_FUNCTION("PFec14-UhEQ", "libScePad", 1, "libScePad", scePadVirtualDeviceDeleteDevice);
    LIB_FUNCTION("pjPCronWdxI", "libScePad", 1, "libScePad",
                 scePadVirtualDeviceDisableButtonRemapping);
    LIB_FUNCTION("LKXfw7VJYqg", "libScePad", 1, "libScePad", scePadVirtualDeviceGetRemoteSetting);
    LIB_FUNCTION("IWOyO5jKuZg", "libScePad", 1, "libScePad", scePadVirtualDeviceInsertData);
    LIB_FUNCTION("KY0hSB+Uyfo", "libScePad", 1, "libScePad", Func_298D21481F94C9FA);
    LIB_FUNCTION("UeUUvNOgXKU", "libScePad", 1, "libScePad", Func_51E514BCD3A05CA5);
    LIB_FUNCTION("ickjfjk9okM", "libScePad", 1, "libScePad", Func_89C9237E393DA243);
    LIB_FUNCTION("7xA+hFtvBCA", "libScePad", 1, "libScePad", Func_EF103E845B6F0420);
};

} // namespace Libraries::Pad
