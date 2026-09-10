// SPDX-License-Identifier: GPL-2.0-or-later
// Uses production controller/menu/user-manager objects and real SDL virtual pads.
// Only persistence, logging, guest clock, capture and binding reset are test adapters.
#include <cstdio>
#include <cstdlib>
#include <SDL3/SDL.h>
#include <imgui.h>
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/user_settings.h"
#include "input/controller.h"
#include "input/profile_menu.h"
#include "imgui/imgui_layer.h"

void Check(bool ok, const char* why) { if (!ok) { fprintf(stderr, "FAIL: %s (%s)\n", why, SDL_GetError()); exit(1); } }
void assert_fail_debug_msg(const char* message) { fprintf(stderr,"%s\n",message); abort(); }
void assert_fail_impl() { abort(); }
[[noreturn]] void unreachable_impl() { abort(); }
namespace Common { std::string GetCurrentThreadName() { return "test"; } }
namespace Common::Log { std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS; }
UserSettingsImpl::UserSettingsImpl() = default;
UserSettingsImpl::~UserSettingsImpl() = default;
std::shared_ptr<UserSettingsImpl> UserSettingsImpl::GetInstance() { static auto p = std::make_shared<UserSettingsImpl>(); return p; }
EmulatorSettingsImpl::EmulatorSettingsImpl() = default;
EmulatorSettingsImpl::~EmulatorSettingsImpl() = default;
std::shared_ptr<EmulatorSettingsImpl> EmulatorSettingsImpl::GetInstance() { static auto p = std::make_shared<EmulatorSettingsImpl>(); return p; }
namespace Libraries::Kernel { u64 PS4_SYSV_ABI sceKernelGetProcessTime() { static u64 t; return ++t; } }
int logins{}, capture{};
namespace Libraries::UserService { void AddUserServiceEvent(const OrbisUserServiceEvent) { ++logins; } }
ImGui::Layer* layer{};
void ImGui::Layer::AddLayer(Layer* p) { Check(!layer, "one UI layer"); layer = p; }
void ImGui::Layer::RemoveLayer(Layer* p) { Check(p == layer, "same UI layer"); layer = nullptr; }
namespace ImGui::Core {
void AcquireGamepadInputCapture() { ++capture; Check(capture == 1, "capture not nested"); }
void ReleaseGamepadInputCapture() { --capture; Check(capture == 0, "capture balanced"); }
}
namespace Input { void ReleaseAllInputs() { for (int i=0;i<5;++i) (*Common::Singleton<GameControllers>::Instance())[i]->ClearInput(); } }
using namespace Input;
void Drain() { SDL_Event e; while(SDL_PollEvent(&e)) Profiles::ProcessEvent(e); }
void Key(SDL_Keycode k) { SDL_Event e{}; e.type=SDL_EVENT_KEY_DOWN; e.key.key=k; e.key.down=true; Profiles::ProcessEvent(e); Drain(); }
void Button(SDL_JoystickID id, SDL_GamepadButton b) { SDL_Event e{}; e.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.which=id; e.gbutton.button=b; e.gbutton.down=true; Profiles::ProcessEvent(e); Drain(); }
int main() {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0"); SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0"); SDL_SetHint(SDL_HINT_JOYSTICK_IOKIT, "0");
    Check(SDL_Init(SDL_INIT_GAMEPAD), "SDL init");
    int count{}; auto* pads = SDL_GetGamepads(&count); SDL_free(pads); Check(!count, "physical pads excluded");
    for(int i=0;i<4;++i) UserManagement.GetUsers().user.push_back(User{.user_id=1000+i,.user_name="Player "+std::to_string(i+1),.player_index=i+1});
    auto& c=*Common::Singleton<GameControllers>::Instance();
    Profiles::Register();
    ImGui::CreateContext(); auto& io=ImGui::GetIO(); io.DisplaySize={900,700}; io.DeltaTime=1.0f/60; io.IniFilename=nullptr;
    unsigned char* pixels; int w,h; io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    SDL_JoystickID ids[5]{};
    for(int i=0;i<5;++i) {
        SDL_VirtualJoystickDesc desc{}; SDL_INIT_INTERFACE(&desc); desc.type=SDL_JOYSTICK_TYPE_GAMEPAD; desc.naxes=6; desc.nbuttons=15; desc.axis_mask=63; desc.button_mask=32767; desc.name="Profile regression pad";
        ids[i]=SDL_AttachVirtualJoystick(&desc); Check(ids[i], "attach virtual pad");
        c.TryOpenSDLControllers(); Drain();
        if(i==0) Check(!Profiles::IsOpen(), "first pad does not prompt");
        else if(i<4) {
            Check(Profiles::IsOpen() && capture==1, "additional pad opens shared menu");
            ImGui::NewFrame(); layer->Draw(); ImGui::Render();
            ImGui::NewFrame(); layer->Draw(); ImGui::Render(); Check(ImGui::GetDrawData()->TotalVtxCount>0, "shared menu draws");
            Key(SDLK_ESCAPE); Check(!Profiles::IsOpen() && !capture, "escape closes");
        } else Check(!SDL_GetGamepadFromID(ids[i]), "fifth handle closed");
    }
    Check(logins==4, "exactly four login events");
    auto* original0=c[0]; auto* original1=c[1];
    c[0]->Button(Libraries::Pad::OrbisPadButtonDataOffset::Cross,true);
    Button(ids[1],SDL_GAMEPAD_BUTTON_GUIDE); Check(Profiles::IsOpen(),"Guide opens");
    Button(ids[1],SDL_GAMEPAD_BUTTON_DPAD_UP); Button(ids[1],SDL_GAMEPAD_BUTTON_SOUTH);
    Check(c.GetGamepadIndexFromJoystickId(ids[1])==0 && c.GetGamepadIndexFromJoystickId(ids[0])==1,"occupied profiles swap");
    Check(c[0]==original0 && c[1]==original1 && c[0]->user_id==1000 && c[1]->user_id==1001,"guest handles/profiles stable");
    State states[64]; bool connected; int connections;
    int n=c[0]->ReadStates(states,64,&connected,&connections); Check(n>0,"neutral state queued");
    for(int i=0;i<n;++i) Check(states[i].buttonsState==Libraries::Pad::OrbisPadButtonDataOffset::None,"no stale pressed buttons");
    Check(logins==4,"swap no duplicate logins");
    Button(ids[1],SDL_GAMEPAD_BUTTON_EAST); Check(!Profiles::IsOpen(),"B closes");
    Key(SDLK_F2); Check(Profiles::IsOpen(),"F2 opens same menu");
    Check(SDL_DetachVirtualJoystick(ids[4]),"remove extra pad");
    Check(SDL_DetachVirtualJoystick(ids[0]),"remove assigned pad"); c.TryOpenSDLControllers(); Drain();
    Check(!c.AssignDeviceToProfile(ids[0],0),"stale assignment rejected");
    Check(!c.AssignDeviceToProfile(ids[1],4),"invalid slot rejected");
    Check(c.AssignDeviceToProfile(ids[1],1),"assign empty profile");
    Check(c.GetGamepadIndexFromJoystickId(ids[1])==1 && !c[0]->m_sdl_gamepad,"move into empty profile");
    for(int i=1;i<4;++i) Check(SDL_DetachVirtualJoystick(ids[i]),"remove remaining pad");
    c.TryOpenSDLControllers(); Drain();
    ImGui::NewFrame(); layer->Draw(); ImGui::Render();
    Key(SDLK_F2); Check(!Profiles::IsOpen() && !capture,"empty menu closes");
    Profiles::Unregister(); ImGui::DestroyContext(); SDL_Quit();
    puts("PASS: shared menu, 1-5 controllers, Guide/F2, draw, swap, stable handles/users, neutral queues, unplug/stale devices, capture balance");
}
