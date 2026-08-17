// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cmath>
#include <string>
#include <string_view>

#include "app_content.h"
#include "common/assert.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/libraries/app_content/app_content_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/libs.h"
#include "core/libraries/system/systemservice.h"

namespace Libraries::AppContent {

struct AddContInfo {
    char entitlement_label[ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE];
    OrbisAppContentAddcontDownloadStatus status;
    OrbisAppContentGetEntitlementKey key;
};

static std::array<AddContInfo, ORBIS_APP_CONTENT_INFO_LIST_MAX_SIZE> addcont_info = {{
    {"0000000000000000",
     OrbisAppContentAddcontDownloadStatus::Installed,
     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00}},
}};

static s32 sdk_ver = 0;
static s32 addcont_count = 0;
static std::string title_id;
static bool is_initialized = false;

// LBP3 contains the payload for these legacy LBP1/LBP2 products in the base game. On PS4,
// ownership migration exposes their unified entitlement labels without downloadable addcont data.
// Keep this list scoped to the exact CUSA00063 v1.26 catalog; ordinary PS4 addcont continues to be
// discovered from installed param.sfo files below.
static constexpr std::array<std::string_view, 53> Lbp3LegacyEntitlements = {
    "LBPDLC2KADCK0001", "LBPDLC2KADCO0001", "LBPDLC2KADCO0002", "LBPDLC2KADCO0003",
    "LBPDLC2KADCO0004", "LBPDLCCOMPCO0001", "LBPDLCCOMPCO0003", "LBPDLCCOMPCO0004",
    "LBPDLCCPSFCK0001", "LBPDLCCPSFCO0001", "LBPDLCCPSFCO0002", "LBPDLCCPSFCO0003",
    "LBPDLCCPSFCO0004", "LBPDLCKMGSCK0001", "LBPDLCKMGSCO0002", "LBPDLCKMGSCO0003",
    "LBPDLCKMGSCO0004", "LBPDLCKMGSCO0005", "LBPDLCKMGSLK0001", "LBPDLCORIGCK0001",
    "LBPDLCORIGCO0001", "LBPDLCORIGCO0002", "LBPDLCORIGCO0003", "LBPDLCORIGCO0004",
    "LBPDLCORIGCO0005", "LBPDLCORIGCO0006", "LBPDLCORIGCO0007", "LBPDLCORIGCO0008",
    "LBPDLCORIGCO0009", "LBPDLCORIGCO0010", "LBPDLCORIGCO0011", "LBPDLCORIGCO0012",
    "LBPDLCORIGCO0013", "LBPDLCORIGLK0001", "LBPDLCORIGLK0002", "LBPDLCRARECO0001",
    "LBPDLCRARECO0002", "LBPDLCSONYCK0002", "LBPDLCSONYCK0003", "LBPDLCSONYCO0001",
    "LBPDLCSONYCO0002", "LBPDLCSONYCO0003", "LBPDLCSONYCO0004", "LBPDLCSONYCO0005",
    "LBPDLCSONYCO0006", "LBPDLCSONYCO0007", "LBPDLCSONYCO0008", "LBPDLCSONYCO0011",
    "LBPDLCSONYCO0012", "LBPDLCSONYMP0001", "LBPDLCSONYMP0002", "LBPDLCSONYMP0003",
    "LBPDLCSONYST0001",
};

// patch_001.farc in CUSA00063 v1.26 contains 659 unique DLCt records. Every record names a shipped
// .edat and carries one direct ContentID. Grouping the numeric suffixes keeps the exact catalog
// auditable without granting IDs that are only mentioned by UI/config strings elsewhere.
struct Lbp3EntitlementGroup {
    std::string_view prefix;
    std::string_view suffixes;
};

static constexpr std::array<Lbp3EntitlementGroup, 128> Lbp3EmbeddedEntitlementGroups = {{
    {"LBPDLC7ELECO", "0001"},
    {"LBPDLCBBDWCK", "0001 0002 0003 0004"},
    {"LBPDLCBBDWCO",
     "0001 0002 0003 0004 0005 0006 0007 0008 0009 0010 0011 0012 0013 0014 0015 0016"},
    {"LBPDLCBIOSCK", "0001"},
    {"LBPDLCBIOSCO", "0001 0002 0003 0004 0005"},
    {"LBPDLCBIOSMP", "0001"},
    {"LBPDLCBSGACK", "0001"},
    {"LBPDLCBSGACO", "0001 0002 0003 0004"},
    {"LBPDLCBTTFCO", "0001 0002 0003 0004 0005 0006 0007 0008"},
    {"LBPDLCBTTFCP", "0001 0002"},
    {"LBPDLCBTTFLK", "0001"},
    {"LBPDLCBTWACP", "0001"},
    {"LBPDLCCNATCO", "0001 0002 0003 0004 0005"},
    {"LBPDLCCNATCP", "0001"},
    {"LBPDLCCNATLK", "0001"},
    {"LBPDLCCNSUCK", "0001"},
    {"LBPDLCCNSUCO", "0001 0002 0003 0004"},
    {"LBPDLCDSALCK", "0001"},
    {"LBPDLCDSALCO", "0001 0002 0003 0006"},
    {"LBPDLCDSBHCK", "0001"},
    {"LBPDLCDSBHCO", "0001 0002 0003 0004 0005"},
    {"LBPDLCDSFRCK", "0001"},
    {"LBPDLCDSFRCO", "0001 0002 0003 0004 0005 0006 0007"},
    {"LBPDLCDSFWMP", "0001"},
    {"LBPDLCDSGDCO", "0001 0002 0003 0004 0005"},
    {"LBPDLCDSGDCP", "0001"},
    {"LBPDLCDSINCK", "0001"},
    {"LBPDLCDSINCO", "0001 0002 0003 0004 0005"},
    {"LBPDLCDSINLK", "0001"},
    {"LBPDLCDSIOCK", "0001"},
    {"LBPDLCDSIOCO", "0001 0002 0003 0004 0005 0006"},
    {"LBPDLCDSMFMP", "0001"},
    {"LBPDLCDSMICO", "0001 0002 0003 0004 0005"},
    {"LBPDLCDSMICP", "0001"},
    {"LBPDLCDSMUCK", "0001 0002 0003"},
    {"LBPDLCDSMUCO", "0001 0002 0003 0004 0005 0006 0008 0009 0010 0011 0012 0013 0014 0015"},
    {"LBPDLCDSMULK", "0001"},
    {"LBPDLCDSNBCK", "0001"},
    {"LBPDLCDSNBCO", "0001 0002 0003 0004"},
    {"LBPDLCDSNBLK", "0001"},
    {"LBPDLCDSPCCK", "0001"},
    {"LBPDLCDSPCCO", "0001 0002 0003 0004 0005 0006"},
    {"LBPDLCDSPCLK", "0001"},
    {"LBPDLCDSPRCK", "0001 0002"},
    {"LBPDLCDSPRCO", "0001 0002 0003 0004 0005 0006 0007 0008"},
    {"LBPDLCDSTRCO", "0001"},
    {"LBPDLCDSTRMP", "0001 0003"},
    {"LBPDLCDSTSCK", "0001 0002 0003"},
    {"LBPDLCDSTSCO", "0001 0002 0003 0004 0005 0006 0007 0008 0009 0010 0011 0012 0013"},
    {"LBPDLCDSTSLK", "0001"},
    {"LBPDLCEADACK", "0001"},
    {"LBPDLCEADACO", "0001 0002 0003 0004 0005"},
    {"LBPDLCEADAMP", "0001"},
    {"LBPDLCEADSCO", "0001"},
    {"LBPDLCEAMCCO", "0001 0002 0003 0004 0005"},
    {"LBPDLCEAMCCP", "0001"},
    {"LBPDLCEAMECK", "0001 0002"},
    {"LBPDLCEAMECO", "0001 0002 0003 0004 0005 0006 0007 0008 0009"},
    {"LBPDLCEAPZCO", "0001 0002 0003 0004 0005"},
    {"LBPDLCEAPZCP", "0001"},
    {"LBPDLCFLV2CO", "0001"},
    {"LBPDLCGILMSK", "0001"},
    {"LBPDLCHGJDCO", "0001"},
    {"LBPDLCINSOCK", "0001"},
    {"LBPDLCINSOCO", "0001 0002"},
    {"LBPDLCKLEICO", "0001 0002 0003 0004 0005"},
    {"LBPDLCKLEICP", "0001"},
    {"LBPDLCKMGSCK", "0002 0003"},
    {"LBPDLCKMGSCO", "0006 0007 0008 0009 0010 0011 0012 0013"},
    {"LBPDLCLBPCUPCK", "01 02 03 04 05 06 07 08"},
    {"LBPDLCLBPCUPCO", "01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 "
                       "26 27 28 29 30 31 32 33 34"},
    {"LBPDLCLBPCUPCP", "01"},
    {"LBPDLCLEV5CK", "0001"},
    {"LBPDLCLEV5CO", "0001 0002 0003 0004"},
    {"LBPDLCMARVCK", "0001 0002 0003 0004 0005 0006"},
    {"LBPDLCMARVCO", "0001 0002 0003 0004 0005 0006 0007 0008 0009 0010 0011 0012 0013 0014 0015 "
                     "0016 0017 0018 0019 0020 0021 0022 0023 0024 0025 0026 0027 0028 0029 0030"},
    {"LBPDLCMARVLK", "0001"},
    {"LBPDLCMCBESK", "0001"},
    {"LBPDLCMOVELK", "0001"},
    {"LBPDLCNBDSCK", "0001"},
    {"LBPDLCNBDSCO", "0001 0002 0003 0004 0005"},
    {"LBPDLCNBLNMP", "0001"},
    {"LBPDLCNBNCCK", "0001"},
    {"LBPDLCNBNCCO", "0001 0002 0003 0004 0005 0006 0007"},
    {"LBPDLCNBSCCK", "0001"},
    {"LBPDLCNBSCCO", "0001 0002 0003 0004 0005 0006"},
    {"LBPDLCNBTKCK", "0001"},
    {"LBPDLCNBTKCO", "0001 0002 0003 0004 0005 0006"},
    {"LBPDLCNDOGCK", "0001"},
    {"LBPDLCNDOGCO", "0001 0002"},
    {"LBPDLCNISBCK", "0001"},
    {"LBPDLCNISBCO", "0001 0002 0003 0004 0005"},
    {"LBPDLCNISBLK", "0001"},
    {"LBPDLCODDWCO", "0001 0002 0003"},
    {"LBPDLCORIGCK", "0002 0003 0004 0005 0006 0007 0008 0009 0010 0011 0012 0013 0014 0015 0016 "
                     "0017 0018 0019 0020 0021 0023"},
    {"LBPDLCORIGCO",
     "0014 0015 0016 0017 0018 0019 0020 0021 0022 0023 0024 0025 0026 0027 0028 0029 0030 0031 "
     "0032 0033 0034 0035 0036 0037 0038 0039 0040 0041 0042 0043 0044 0045 0046 0047 0048 0049 "
     "0050 0051 0052 0053 0054 0055 0056 0057 0058 0059 0060 0061 0062 0063 0064 0065 0066 0067 "
     "0068 0069 0070 0071 0072 0073 0074 0075 0076 0077 0078 0079 0080 0081 0082 0083 0084 0085 "
     "0086 0087 0088 0089 0090 0091 0092 0093 0094 0095 0096 0101 0102 0103 0104 0105 0106 0107 "
     "0108 0109 0110 0111 0112 0113 0114 0115 0116 0117 0118 0119 0120 0124 0125 0126 0127 0128 "
     "0129 0130 0131 0132 0133 0134 0135 0136"},
    {"LBPDLCORIGCP", "0001"},
    {"LBPDLCORIGLK", "0003 0004 0005 0006 0007 0008 0009 0010 0011 0013 0014 0015"},
    {"LBPDLCORIGMP", "0002 0003 0008 0010"},
    {"LBPDLCPAWSMP", "0001"},
    {"LBPDLCRARECO", "0003 0004 0005 0006 0007 0008 0009 0010"},
    {"LBPDLCSEGACK", "0001"},
    {"LBPDLCSEGACO", "0001 0002 0003 0004 0005"},
    {"LBPDLCSNYPCK", "0001 0002"},
    {"LBPDLCSNYPCO", "0001 0002 0003 0004 0005 0006 0007 0008"},
    {"LBPDLCSONYCK", "0004 0005 0006 0007 0008 0009 0010 0011"},
    {"LBPDLCSONYCO",
     "0013 0014 0015 0016 0017 0018 0019 0020 0021 0022 0023 0024 0026 0027 0028 0029 0030 0031 "
     "0032 0033 0034 0035 0036 0037 0038 0039 0040 0041 0042 0043 0044 0045 0046 0050 0051 0052 "
     "0054 0055 0056 0057 0058 0059 0060 0061 0062 0063 0064 0065 0066 0067 0068 0069"},
    {"LBPDLCSONYCP", "0001"},
    {"LBPDLCSONYMP", "0004 0005 0006 0007 0009 0010"},
    {"LBPDLCSONYSK", "0001 0002"},
    {"LBPDLCSPAAMP", "0001"},
    {"LBPDLCSQEXCK", "0001"},
    {"LBPDLCSQEXCO", "0001 0002 0003 0004 0005"},
    {"LBPDLCSUPUCK", "0001"},
    {"LBPDLCTARGCO", "0001"},
    {"LBPDLCTESTSK", "0001 0002 0003"},
    {"LBPDLCTLOUCO", "0001 0002"},
    {"LBPDLCTLOUMP", "0001"},
    {"LBPDLCTMNTCK", "0001 0002"},
    {"LBPDLCTMNTCO", "0001 0002 0003 0004 0005 0006 0007 0008 0009 0010 0011"},
    {"LBPDLCUBACCO", "0001 0002"},
    {"LBPDLCWARNCK", "0001"},
    {"LBPDLCWARNCO", "0001 0002 0003 0004"},
    {"LBPDLCWBDCCK", "0001 0002 0003 0004"},
    {"LBPDLCWBDCCO", "0001 0002 0003 0004 0005 0006 0007 0008 0009 0010 0011 0012 0013 0014 0015 "
                     "0016 0017 0018 0019 0020"},
    {"LBPDLCWBDCLK", "0001 0002"},
    {"LBPDLCYHODCO", "0001"},
}};

static constexpr std::array<std::string_view, 1> Lbp3EmbeddedLiteralEntitlements = {
    "LBPDLCCHALLENGES",
};

static bool AddEntitlement(std::string_view entitlement_label,
                           OrbisAppContentAddcontDownloadStatus status) {
    for (s32 i = 0; i < addcont_count; ++i) {
        if (entitlement_label == addcont_info[i].entitlement_label) {
            return false;
        }
    }

    if (addcont_count >= static_cast<s32>(addcont_info.size())) {
        LOG_WARNING(Lib_AppContent, "Cannot add entitlement {}: info list is full",
                    entitlement_label);
        return false;
    }
    ASSERT_MSG(entitlement_label.size() < ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE,
               "Malformed entitlement label {}", entitlement_label);

    auto& info = addcont_info[addcont_count++];
    info = {};
    entitlement_label.copy(info.entitlement_label, entitlement_label.size());
    info.entitlement_label[entitlement_label.size()] = '\0';
    info.status = status;
    return true;
}

static s32 AddLbp3EmbeddedCatalogEntitlements() {
    constexpr auto status = OrbisAppContentAddcontDownloadStatus::NoExtraData;
    s32 catalog_entries = 0;
    s32 added_entries = 0;

    for (const auto entitlement_label : Lbp3EmbeddedLiteralEntitlements) {
        ++catalog_entries;
        added_entries += AddEntitlement(entitlement_label, status);
    }

    for (const auto& group : Lbp3EmbeddedEntitlementGroups) {
        std::size_t suffix_begin = 0;
        while (suffix_begin < group.suffixes.size()) {
            const std::size_t suffix_end = group.suffixes.find(' ', suffix_begin);
            const std::string_view suffix =
                suffix_end == std::string_view::npos
                    ? group.suffixes.substr(suffix_begin)
                    : group.suffixes.substr(suffix_begin, suffix_end - suffix_begin);

            std::string entitlement_label{group.prefix};
            entitlement_label.append(suffix.data(), suffix.size());
            ASSERT_MSG(entitlement_label.size() == ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE - 1,
                       "Malformed embedded LBP3 entitlement {}", entitlement_label);
            ++catalog_entries;
            added_entries += AddEntitlement(entitlement_label, status);

            if (suffix_end == std::string_view::npos) {
                break;
            }
            suffix_begin = suffix_end + 1;
        }
    }

    ASSERT_MSG(catalog_entries == 659, "Expected 659 embedded LBP3 DLCt records, got {}",
               catalog_entries);
    return added_entries;
}

static bool HasExtraData(const std::filesystem::path& addon_path) {
    for (const auto& entry : std::filesystem::directory_iterator(addon_path)) {
        const auto filename = entry.path().filename();
        if (filename != "sce_sys" && filename != ".DS_Store") {
            return true;
        }
    }
    return false;
}

int PS4_SYSV_ABI _Z5dummyv() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontDelete() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontEnqueueDownload() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontEnqueueDownloadSp() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontMount(u32 service_label,
                                           const OrbisNpUnifiedEntitlementLabel* entitlement_label,
                                           OrbisAppContentMountPoint* mount_point) {
    if (entitlement_label == nullptr || mount_point == nullptr) {
        return ORBIS_APP_CONTENT_ERROR_PARAMETER;
    }
    LOG_INFO(Lib_AppContent, "called for {}", entitlement_label->data);

    const auto& addon_path = EmulatorSettings.GetAddonInstallDir() / title_id;
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();

    // Determine which loaded additional content this entitlement label is for.
    s32 i = 0;
    while (i < addcont_count) {
        if (strncmp(entitlement_label->data, addcont_info[i].entitlement_label,
                    ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE - 1) == 0) {
            break;
        }
        ++i;
    }

    if (i == addcont_count) {
        // None of the loaded additional content match the entitlement label requested.
        return ORBIS_APP_CONTENT_ERROR_NOT_FOUND;
    }

    // An entitlement with NoExtraData grants access to content embedded in the base game and has
    // no filesystem payload to mount.
    if (addcont_info[i].status == OrbisAppContentAddcontDownloadStatus::NoExtraData) {
        LOG_INFO(Lib_AppContent, "Entitlement {} has no mountable addcont data",
                 entitlement_label->data);
        return ORBIS_APP_CONTENT_ERROR_NOT_FOUND;
    }
    snprintf(mount_point->data, ORBIS_APP_CONTENT_MOUNTPOINT_DATA_MAXSIZE, "/addcont%d", i);

    // Find which directory corresponds to this entitlement
    for (const auto& entry : std::filesystem::directory_iterator(addon_path)) {
        if (!entry.is_directory()) {
            continue;
        }

        // Open the param.sfo in this folder
        PSF* dlc_params = new PSF();
        const auto& param_sfo_path = entry.path() / "sce_sys/param.sfo";
        if (!std::filesystem::exists(param_sfo_path)) {
            // This folder doesn't have a param.sfo
            continue;
        }
        dlc_params->Open(param_sfo_path);

        // Validate the available params
        auto category = dlc_params->GetString("CATEGORY");
        auto content_id = dlc_params->GetString("CONTENT_ID");
        if (!category.has_value() || strncmp(category.value().data(), "ac", 2) != 0 ||
            !content_id.has_value() ||
            content_id.value().length() <= ORBIS_APP_CONTENT_ENTITLEMENT_LABEL_OFFSET) {
            // This folder fails the error checks performed in sceAppContentInitialize.
            continue;
        }

        auto entitlement_id = content_id.value().substr(ORBIS_APP_CONTENT_ENTITLEMENT_LABEL_OFFSET);
        if (strncmp(entitlement_id.data(), entitlement_label->data, entitlement_id.length()) == 0) {
            // We've located the correct folder.
            mnt->Mount(entry.path(), mount_point->data);
            return ORBIS_OK;
        }
    }

    // Hitting this shouldn't be possible, as it would mean the entitlement was loaded,
    // but the folder it was loaded from doesn't exist.
    UNREACHABLE_MSG("Folder for loaded entitlement label {} doesn't exist.",
                    entitlement_label->data);
}

int PS4_SYSV_ABI sceAppContentAddcontShrink() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontUnmount() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAppParamGetInt(OrbisAppContentAppParamId paramId, s32* out_value) {
    if (out_value == nullptr)
        return ORBIS_APP_CONTENT_ERROR_PARAMETER;
    auto* param_sfo = Common::Singleton<PSF>::Instance();
    std::optional<s32> value;
    switch (paramId) {
    case ORBIS_APP_CONTENT_APPPARAM_ID_SKU_FLAG:
        value = ORBIS_APP_CONTENT_APPPARAM_SKU_FLAG_FULL;
        break;
    case ORBIS_APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_1:
        value = param_sfo->GetInteger("USER_DEFINED_PARAM_1");
        break;
    case ORBIS_APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_2:
        value = param_sfo->GetInteger("USER_DEFINED_PARAM_2");
        break;
    case ORBIS_APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_3:
        value = param_sfo->GetInteger("USER_DEFINED_PARAM_3");
        break;
    case ORBIS_APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_4:
        value = param_sfo->GetInteger("USER_DEFINED_PARAM_4");
        break;
    default:
        LOG_ERROR(Lib_AppContent, " paramId = {} paramId is not valid", paramId);
        return ORBIS_APP_CONTENT_ERROR_PARAMETER;
    }
    *out_value = value.value_or(0);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAppParamGetString() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentDownload0Expand() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentDownload0Shrink() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentDownload1Expand() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentDownload1Shrink() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentDownloadDataFormat() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentDownloadDataGetAvailableSpaceKb(OrbisAppContentMountPoint* mountPoint,
                                                              u64* availableSpaceKb) {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    *availableSpaceKb = 1048576;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetAddcontDownloadProgress() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetAddcontInfo(u32 service_label,
                                             const OrbisNpUnifiedEntitlementLabel* entitlementLabel,
                                             OrbisAppContentAddcontInfo* info) {
    LOG_INFO(Lib_AppContent, "called");

    if (entitlementLabel == nullptr || info == nullptr) {
        return ORBIS_APP_CONTENT_ERROR_PARAMETER;
    }

    for (auto i = 0; i < addcont_count; i++) {
        if (strncmp(entitlementLabel->data, addcont_info[i].entitlement_label,
                    ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE - 1) != 0) {
            continue;
        }

        LOG_INFO(Lib_AppContent, "found DLC {}", entitlementLabel->data);

        strncpy(info->entitlement_label.data, addcont_info[i].entitlement_label,
                ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE);
        info->status = addcont_info[i].status;
        return ORBIS_OK;
    }

    return ORBIS_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
}

int PS4_SYSV_ABI sceAppContentGetAddcontInfoList(u32 service_label,
                                                 OrbisAppContentAddcontInfo* list, u32 list_num,
                                                 u32* hit_num) {
    LOG_INFO(Lib_AppContent, "called: list_num={}, available={}", list_num, addcont_count);

    if (list_num == 0 || list == nullptr) {
        if (hit_num == nullptr) {
            return ORBIS_APP_CONTENT_ERROR_PARAMETER;
        }

        *hit_num = addcont_count;
        return ORBIS_OK;
    }

    int dlcs_to_list = addcont_count < list_num ? addcont_count : list_num;
    for (int i = 0; i < dlcs_to_list; i++) {
        strncpy(list[i].entitlement_label.data, addcont_info[i].entitlement_label,
                ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE);
        list[i].status = addcont_info[i].status;
    }

    if (hit_num != nullptr) {
        *hit_num = dlcs_to_list;
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetEntitlementKey(
    u32 service_label, const OrbisNpUnifiedEntitlementLabel* entitlement_label,
    OrbisAppContentGetEntitlementKey* key) {
    LOG_INFO(Lib_AppContent, "called");

    if (entitlement_label == nullptr || key == nullptr) {
        return ORBIS_APP_CONTENT_ERROR_PARAMETER;
    }

    for (int i = 0; i < addcont_count; i++) {
        if (strncmp(entitlement_label->data, addcont_info[i].entitlement_label,
                    ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE - 1) != 0) {
            continue;
        }

        memcpy(key->data, addcont_info[i].key.data, ORBIS_APP_CONTENT_ENTITLEMENT_KEY_SIZE);
        return ORBIS_OK;
    }

    return ORBIS_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
}

int PS4_SYSV_ABI sceAppContentGetRegion() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentInitialize(const OrbisAppContentInitParam* initParam,
                                         OrbisAppContentBootParam* bootParam) {
    if (sdk_ver >= Common::ElfInfo::FW_150 && is_initialized) {
        LOG_ERROR(Lib_AppContent, "Already initialized");
        return ORBIS_APP_CONTENT_ERROR_BUSY;
    }

    LOG_WARNING(Lib_AppContent, "(DUMMY) called");
    is_initialized = true;
    auto* param_sfo = Common::Singleton<PSF>::Instance();

    const auto addons_dir = EmulatorSettings.GetAddonInstallDir();
    if (const auto value = param_sfo->GetString("TITLE_ID"); value.has_value()) {
        title_id = *value;
    } else {
        UNREACHABLE_MSG("Failed to get TITLE_ID");
    }
    const auto addon_path = addons_dir / title_id;
    if (std::filesystem::exists(addon_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(addon_path)) {
            if (!entry.is_directory()) {
                continue;
            }
            // Look for a param.sfo in the additional content directory.
            const auto& param_sfo_path = entry.path() / "sce_sys/param.sfo";
            if (!std::filesystem::exists(param_sfo_path)) {
                LOG_WARNING(Lib_AppContent, "Additonal content folder {} has no param.sfo",
                            entry.path().filename().string());
                continue;
            }

            // Open the param.sfo, make sure it's actually for additional content.
            PSF* dlc_params = new PSF();
            dlc_params->Open(param_sfo_path);

            auto category = dlc_params->GetString("CATEGORY");
            if (category.has_value() && strncmp(category.value().data(), "ac", 2) == 0) {
                // We've located additional content. Find the entitlement id from the content id.
                auto content_id = dlc_params->GetString("CONTENT_ID");
                if (!content_id.has_value()) {
                    LOG_WARNING(Lib_AppContent,
                                "Additonal content {} param.sfo is missing CONTENT_ID",
                                entry.path().filename().string());
                    continue;
                }

                // content id's have consistent formatting, so this will always work.
                // They follow the format UPXXXX-CUSAXXXXX_XX-entitlement
                if (content_id.value().length() <= ORBIS_APP_CONTENT_ENTITLEMENT_LABEL_OFFSET) {
                    LOG_WARNING(Lib_AppContent,
                                "Additonal content {} param.sfo has malformed CONTENT_ID",
                                entry.path().filename().string());
                    continue;
                }
                auto entitlement_id =
                    content_id.value().substr(ORBIS_APP_CONTENT_ENTITLEMENT_LABEL_OFFSET);
                LOG_INFO(Lib_AppContent, "Entitlement {} found", entitlement_id);

                // Entitlement-only add-ons have no downloadable payload outside sce_sys.
                // Report NoExtraData for them; Installed describes add-ons with mounted data.
                AddEntitlement(entitlement_id,
                               HasExtraData(entry.path())
                                   ? OrbisAppContentAddcontDownloadStatus::Installed
                                   : OrbisAppContentAddcontDownloadStatus::NoExtraData);
            } else {
                LOG_WARNING(Lib_AppContent, "Additonal content folder {} is not additional content",
                            entry.path().filename().string());
                continue;
            }
        }
    }

    if (title_id == "CUSA00063") {
        s32 legacy_count = 0;
        for (const auto entitlement_label : Lbp3LegacyEntitlements) {
            legacy_count += AddEntitlement(entitlement_label,
                                           OrbisAppContentAddcontDownloadStatus::NoExtraData);
        }
        const s32 embedded_count = AddLbp3EmbeddedCatalogEntitlements();
        LOG_INFO(Lib_AppContent,
                 "Exposed {} migrated and {} of 659 direct embedded LBP DLCt entitlements ({} "
                 "total addcont entries)",
                 legacy_count, embedded_count, addcont_count);
    }

    if (addcont_count > 0) {
        SystemService::OrbisSystemServiceEvent event{};
        event.event_type = SystemService::OrbisSystemServiceEventType::EntitlementUpdate;
        event.service_entitlement_update.userId = 0;
        event.service_entitlement_update.np_service_label = 0;
        SystemService::PushSystemServiceEvent(event);
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentRequestPatchInstall() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentSmallSharedDataFormat() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentSmallSharedDataGetAvailableSpaceKb() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentSmallSharedDataMount() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentSmallSharedDataUnmount() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentTemporaryDataFormat() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentTemporaryDataGetAvailableSpaceKb(
    const OrbisAppContentMountPoint* mountPoint, u64* availableSpaceKb) {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    *availableSpaceKb = 1048576;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentTemporaryDataMount() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentTemporaryDataMount2(OrbisAppContentTemporaryDataOption option,
                                                  OrbisAppContentMountPoint* mountPoint) {
    if (mountPoint == nullptr) {
        return ORBIS_APP_CONTENT_ERROR_PARAMETER;
    }
    static constexpr std::string_view TmpMount = "/temp0";
    TmpMount.copy(mountPoint->data, TmpMount.size());
    mountPoint->data[TmpMount.size()] = '\0';
    LOG_INFO(Lib_AppContent, "sceAppContentTemporaryDataMount2: option = {}, mountPoint = {}",
             option, mountPoint->data);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentTemporaryDataUnmount() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetPftFlag() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_C59A36FF8D7C59DA() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontEnqueueDownloadByEntitlementId() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontMountByEntitlementId() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetAddcontInfoByEntitlementId() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetAddcontInfoListByIroTag() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetDownloadedStoreCountry() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    ASSERT_MSG(Libraries::Kernel::sceKernelGetCompiledSdkVersion(&sdk_ver) == 0,
               "Failed to get SDK version");

    LIB_FUNCTION("AS45QoYHjc4", "libSceAppContent", 1, "libSceAppContentUtil", _Z5dummyv);
    LIB_FUNCTION("ZiATpP9gEkA", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAddcontDelete);
    LIB_FUNCTION("7gxh+5QubhY", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAddcontEnqueueDownload);
    LIB_FUNCTION("TVM-aYIsG9k", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAddcontEnqueueDownloadSp);
    LIB_FUNCTION("VANhIWcqYak", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAddcontMount);
    LIB_FUNCTION("D3H+cjfzzFY", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAddcontShrink);
    LIB_FUNCTION("3rHWaV-1KC4", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAddcontUnmount);
    LIB_FUNCTION("99b82IKXpH4", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAppParamGetInt);
    LIB_FUNCTION("+OlXCu8qxUk", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAppParamGetString);
    LIB_FUNCTION("gpGZDB4ZlrI", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentDownload0Expand);
    LIB_FUNCTION("S5eMvWnbbXg", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentDownload0Shrink);
    LIB_FUNCTION("B5gVeVurdUA", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentDownload1Expand);
    LIB_FUNCTION("kUeYucqnb7o", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentDownload1Shrink);
    LIB_FUNCTION("CN7EbEV7MFU", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentDownloadDataFormat);
    LIB_FUNCTION("Gl6w5i0JokY", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentDownloadDataGetAvailableSpaceKb);
    LIB_FUNCTION("5bvvbUSiFs4", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentGetAddcontDownloadProgress);
    LIB_FUNCTION("m47juOmH0VE", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentGetAddcontInfo);
    LIB_FUNCTION("xnd8BJzAxmk", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentGetAddcontInfoList);
    LIB_FUNCTION("XTWR0UXvcgs", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentGetEntitlementKey);
    LIB_FUNCTION("74-1x3lyZK8", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentGetRegion);
    LIB_FUNCTION("R9lA82OraNs", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentInitialize);
    LIB_FUNCTION("bVtF7v2uqT0", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentRequestPatchInstall);
    LIB_FUNCTION("9Gq5rOkWzNU", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentSmallSharedDataFormat);
    LIB_FUNCTION("xhb-r8etmAA", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentSmallSharedDataGetAvailableSpaceKb);
    LIB_FUNCTION("QuApZnMo9MM", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentSmallSharedDataMount);
    LIB_FUNCTION("EqMtBHWu-5M", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentSmallSharedDataUnmount);
    LIB_FUNCTION("a5N7lAG0y2Q", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentTemporaryDataFormat);
    LIB_FUNCTION("SaKib2Ug0yI", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentTemporaryDataGetAvailableSpaceKb);
    LIB_FUNCTION("7bOLX66Iz-U", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentTemporaryDataMount);
    LIB_FUNCTION("buYbeLOGWmA", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentTemporaryDataMount2);
    LIB_FUNCTION("bcolXMmp6qQ", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentTemporaryDataUnmount);
    LIB_FUNCTION("xmhnAoxN3Wk", "libSceAppContentPft", 1, "libSceAppContent",
                 sceAppContentGetPftFlag);
    LIB_FUNCTION("xZo2-418Wdo", "libSceAppContentBundle", 1, "libSceAppContent",
                 Func_C59A36FF8D7C59DA);
    LIB_FUNCTION("kJmjt81mXKQ", "libSceAppContentIro", 1, "libSceAppContent",
                 sceAppContentAddcontEnqueueDownloadByEntitlementId);
    LIB_FUNCTION("efX3lrPwdKA", "libSceAppContentIro", 1, "libSceAppContent",
                 sceAppContentAddcontMountByEntitlementId);
    LIB_FUNCTION("z9hgjLd1SGA", "libSceAppContentIro", 1, "libSceAppContent",
                 sceAppContentGetAddcontInfoByEntitlementId);
    LIB_FUNCTION("3wUaDTGmjcQ", "libSceAppContentIro", 1, "libSceAppContent",
                 sceAppContentGetAddcontInfoListByIroTag);
    LIB_FUNCTION("TCqT7kPuGx0", "libSceAppContentSc", 1, "libSceAppContent",
                 sceAppContentGetDownloadedStoreCountry);
};

} // namespace Libraries::AppContent
