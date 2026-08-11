// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <mutex>
#include <queue>

namespace {

std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex request_mutex{};

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};
bool cache_uses_archive{};

u32 MergeSeedCache(const std::filesystem::path& seed_path,
                   const std::filesystem::path& runtime_path) {
    std::error_code error;
    if (!std::filesystem::is_directory(seed_path, error)) {
        return 0;
    }
    std::filesystem::create_directories(runtime_path, error);
    error.clear();

    u32 imported{};
    for (std::filesystem::recursive_directory_iterator it{seed_path, error}, end; it != end;
         it.increment(error)) {
        if (error) {
            LOG_WARNING(Render, "Failed to scan shader seed {}: {}", seed_path.string(),
                        error.message());
            break;
        }

        const auto relative = std::filesystem::relative(it->path(), seed_path, error);
        if (error) {
            error.clear();
            continue;
        }
        const auto destination = runtime_path / relative;
        if (it->is_directory(error)) {
            std::filesystem::create_directories(destination, error);
            error.clear();
            continue;
        }
        if (!it->is_regular_file(error)) {
            error.clear();
            continue;
        }

        std::filesystem::create_directories(destination.parent_path(), error);
        error.clear();
        if (std::filesystem::copy_file(it->path(), destination,
                                       std::filesystem::copy_options::skip_existing, error)) {
            ++imported;
        }
        error.clear();
    }
    return imported;
}

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    for (;;) {
        std::packaged_task<void()> request{};
        {
            std::unique_lock lock{request_mutex};
            Common::CondvarWait(request_cv, lock, stoken, [&] { return !req_queue.empty(); });
            if (req_queue.empty()) {
                if (stoken.stop_requested()) {
                    return;
                }
                continue;
            }
            request = std::move(req_queue.front());
            req_queue.pop();
        }

        if (request.valid()) {
            request();
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    cache_uses_archive = EmulatorSettings.IsPipelineCacheArchived() && !lbp3_layout;
    if (cache_uses_archive) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else if (lbp3_layout) {
        const auto title_root =
            GetUserPath(PathType::CacheDir) / std::string{game_info.GameSerial()};
        seed_cache_path = title_root / "seed" / "spirv" / "v1" / profile_namespace;
        cache_path = title_root / "runtime" / "spirv" / "v1" / profile_namespace;

        std::error_code error;
        std::filesystem::create_directories(seed_cache_path, error);
        error.clear();
        std::filesystem::create_directories(cache_path, error);
        error.clear();
        for (const auto* side : {"seed", "runtime"}) {
            std::filesystem::create_directories(title_root / side / "native" / "vulkan" / "v1",
                                                error);
            error.clear();
            std::filesystem::create_directories(title_root / side / "native" / "mesa" / "v1",
                                                error);
            error.clear();
            std::filesystem::create_directories(title_root / side / "native" / "metal" / "v1",
                                                error);
            error.clear();
        }

        u32 imported{};
        if (const char* shipped_seed_root = std::getenv("SHADPS4_SHADER_SEED_ROOT")) {
            const auto shipped_seed = std::filesystem::path{shipped_seed_root} /
                                      std::string{game_info.GameSerial()} / "spirv" / "v1" /
                                      profile_namespace;
            imported += MergeSeedCache(shipped_seed, cache_path);
        }
        imported += MergeSeedCache(seed_cache_path, cache_path);
        LOG_INFO(Render, "LBP3 shader cache profile {}: imported {} seed files into {}",
                 profile_namespace, imported, cache_path.string());
    } else {
        cache_path = GetUserPath(PathType::CacheDir) / game_info.GameSerial();
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void DataBase::ConfigureLbp3Profile(std::string profile_namespace_) {
    if (opened) {
        LOG_WARNING(Render, "Ignoring late LBP3 shader-cache profile configuration");
        return;
    }
    profile_namespace = std::move(profile_namespace_);
    lbp3_layout = true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    io_worker.request_stop();
    request_cv.notify_all();
    io_worker.join();

    // ProcessIO normally drains the queue before honoring stop. Keep a synchronous fallback so an
    // unexpected worker exit still cannot lose the last shaders of the session.
    for (;;) {
        std::packaged_task<void()> request{};
        {
            std::scoped_lock lock{request_mutex};
            if (req_queue.empty()) {
                break;
            }
            request = std::move(req_queue.front());
            req_queue.pop();
        }
        if (request.valid()) {
            request();
        }
    }
    if (cache_uses_archive) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{[=]() {
            auto path{path_};
            path.replace_extension(GetBlobFileExtension(type));
            if (cache_uses_archive) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), v.data(),
                                           v.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                using namespace Common::FS;
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                if (std::filesystem::exists(path, error)) {
                    return;
                }

                auto temporary_path = path;
                temporary_path += ".tmp";
                {
                    const auto file = IOFile{temporary_path, FileAccessMode::Create};
                    if (!file.IsOpen() || file.Write(v) != v.size()) {
                        LOG_ERROR(Render, "Failed to write shader cache file {}",
                                  temporary_path.string());
                        return;
                    }
                }

                std::filesystem::rename(temporary_path, path, error);
                if (error) {
                    // Another writer may have won the same content-addressed filename.
                    std::filesystem::remove(temporary_path, error);
                }
            }
        }};
        std::scoped_lock lock{request_mutex};
        req_queue.emplace(std::move(request));
    }

    request_cv.notify_one();
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (cache_uses_archive) {
        int index{-1};
        index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&zip_ar, index, &stat);
        v.resize(stat.m_uncomp_size / sizeof(T));
        mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0);
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        if (!file.IsOpen()) {
            return;
        }
        v.resize(file.GetSize() / sizeof(T));
        file.Read(v);
    }
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = cache_uses_archive ? std::filesystem::path{name} : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = cache_uses_archive ? std::filesystem::path{name} : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }

    auto path = cache_uses_archive ? std::filesystem::path{name} : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = cache_uses_archive ? std::filesystem::path{name} : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (cache_uses_archive) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0);
                func(std::move(data));
            }
        }
    } else {
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path}) {
            if (file_name.path().extension().string().ends_with(ext)) {
                using namespace Common::FS;
                const auto& file = IOFile{file_name, FileAccessMode::Read};
                if (file.IsOpen()) {
                    std::vector<u8> data(file.GetSize());
                    file.Read(data);
                    func(std::move(data));
                }
            }
        }
    }
}

void DataBase::FinishPreload() {
    if (cache_uses_archive) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage
