// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <dlfcn.h>
#include <Cocoa/Cocoa.h>
#include <SDL3/SDL_video.h>

#include "sdl_window.h"

namespace Frontend {

namespace {

using LSGetCurrentApplicationASN = CFTypeRef (*)();
using LSSetApplicationInformationItem = OSStatus (*)(int, CFTypeRef, CFStringRef, CFStringRef,
                                                      CFDictionaryRef*);

bool SetLaunchServicesDisplayName(NSString* name) {
    constexpr int CurrentSession = -2;
    constexpr const char* ApplicationServicesPath =
        "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";

    void* application_services = dlopen(ApplicationServicesPath, RTLD_LAZY | RTLD_LOCAL);
    if (application_services == nullptr) {
        return false;
    }

    CFBundleRef launch_services =
        CFBundleGetBundleWithIdentifier(CFSTR("com.apple.LaunchServices"));
    if (launch_services == nullptr) {
        dlclose(application_services);
        return false;
    }

    const auto get_current_asn = reinterpret_cast<LSGetCurrentApplicationASN>(
        CFBundleGetFunctionPointerForName(launch_services, CFSTR("_LSGetCurrentApplicationASN")));
    const auto set_information = reinterpret_cast<LSSetApplicationInformationItem>(
        CFBundleGetFunctionPointerForName(launch_services,
                                          CFSTR("_LSSetApplicationInformationItem")));
    const auto display_name_key = reinterpret_cast<CFStringRef*>(
        CFBundleGetDataPointerForName(launch_services, CFSTR("_kLSDisplayNameKey")));

    bool renamed = false;
    if (get_current_asn != nullptr && set_information != nullptr && display_name_key != nullptr &&
        *display_name_key != nullptr) {
        const CFTypeRef asn = get_current_asn();
        renamed = asn != nullptr &&
                  set_information(CurrentSession, asn, *display_name_key,
                                  reinterpret_cast<CFStringRef>(name), nullptr) == noErr;
    }

    dlclose(application_services);
    return renamed;
}

} // namespace

void SetMacOSProcessName(std::string_view application_name) {
    @autoreleasepool {
        NSString* name = [[[NSString alloc] initWithBytes:application_name.data()
                                                   length:application_name.size()
                                                 encoding:NSUTF8StringEncoding] autorelease];
        if (name.length != 0) {
            [NSProcessInfo.processInfo setProcessName:name];
            SetLaunchServicesDisplayName(name);
        }
    }
}

void SetWindowIcon(SDL_Window* window, const std::vector<u8>& png) {
    @autoreleasepool {
        NSData* pngData = [NSData dataWithBytes:png.data() length:png.size()];
        NSImage* baseIcon = [[[NSImage alloc] initWithData:pngData] autorelease];

        // Transform the icon to match native look-and-feel.
        constexpr double ScaleFactor = 13.0 / 16.0;
        constexpr double CornerRadiusFactor = 22.0 / 100.0;

        const double baseIconWidth = baseIcon.size.width;
        const double baseIconHeight = baseIcon.size.height;
        const double iconWidth = baseIconWidth * ScaleFactor;
        const double iconHeight = baseIconHeight * ScaleFactor;
        const double iconX = (baseIconWidth - iconWidth) / 2.0;
        const double iconY = (baseIconHeight - iconHeight) / 2.0;
        const double cornerRadiusX = iconWidth * CornerRadiusFactor;
        const double cornerRadiusY = iconHeight * CornerRadiusFactor;

        NSRect bounds = NSMakeRect(iconX, iconY, iconWidth, iconHeight);
        NSBezierPath* maskPath = [NSBezierPath bezierPathWithRoundedRect:bounds
                                                                 xRadius:cornerRadiusX
                                                                 yRadius:cornerRadiusY];

        NSImage* nativeIcon = [[[NSImage alloc] initWithSize:baseIcon.size] autorelease];
        [nativeIcon lockFocus];
        [maskPath addClip];
        [baseIcon drawInRect:bounds
                    fromRect:NSZeroRect
                   operation:NSCompositingOperationSourceOver
                    fraction:1.0f];
        [nativeIcon unlockFocus];

        [NSApp setApplicationIconImage:nativeIcon];
    }
}

} // namespace Frontend
