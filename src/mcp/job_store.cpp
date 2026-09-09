#include "job_store.hpp"

#include <openssl/rand.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace gptimage {

namespace {

std::string mime_for_ext(const std::string& ext) {
    if (ext == "webp")               return "image/webp";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    return "image/png";
}

std::string ext_for_mime(const std::string& mime) {
    if (mime == "image/webp") return "webp";
    if (mime == "image/jpeg") return "jpeg";
    return "png";
}

// Drop the oldest files once the directory holds more than `max_files`. A render
// directory that grows without bound is a slow way to take a box down, and on a
// shared host it takes everything else with it. 0 disables the cap.
void prune_oldest(const std::filesystem::path& dir, int max_files) {
    if (max_files <= 0) return;
    std::error_code ec;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return;
        if (!e.is_regular_file(ec) || ec) continue;
        const auto t = e.last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        files.emplace_back(t, e.path());
    }
    if (static_cast<int>(files.size()) <= max_files) return;
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    const size_t drop = files.size() - static_cast<size_t>(max_files);
    for (size_t i = 0; i < drop; ++i) {
        std::filesystem::remove(files[i].second, ec);
        if (ec) { ec.clear(); continue; }
        spdlog::info("render store: pruned {}", files[i].second.string());
    }
}

}  // namespace

std::vector<std::filesystem::path> save_render(
    const std::filesystem::path& dir, const std::string& job_id,
    const std::vector<GeneratedImage>& images, int max_files) {
    std::vector<std::filesystem::path> written;
    if (dir.empty() || images.empty()) return written;

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        spdlog::warn("render store: cannot create {}: {}", dir.string(), ec.message());
        return written;
    }

    for (size_t i = 0; i < images.size(); ++i) {
        const auto bytes = base64_decode(images[i].b64);
        if (bytes.empty()) {
            spdlog::warn("render store: {}-{} decoded to nothing, not saved", job_id, i);
            continue;
        }
        // Same name the hosted URL uses, so the HTTP route resolves a path
        // straight from the request with nothing to look up.
        const auto path =
            dir / (job_id + "-" + std::to_string(i) + "." + ext_for_mime(images[i].mime));
        std::ofstream os(path, std::ios::binary | std::ios::trunc);
        if (!os) {
            spdlog::warn("render store: cannot open {} for writing", path.string());
            continue;
        }
        os.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
        os.close();
        if (!os) {
            spdlog::warn("render store: write failed for {}", path.string());
            std::filesystem::remove(path, ec);
            ec.clear();
            continue;
        }
        spdlog::info("render store: saved {} ({} bytes)", path.string(), bytes.size());
        written.push_back(path);
    }

    if (!written.empty()) prune_oldest(dir, max_files);
    return written;
}

std::optional<StoredImage> load_render(
    const std::filesystem::path& dir, const std::string& job_id, size_t index,
    const std::string& ext) {
    if (dir.empty()) return std::nullopt;
    // job_id is matched as job_[0-9a-f]+ and ext against a fixed set upstream, so
    // neither can escape the directory. Rebuild the name rather than trusting a
    // caller-supplied path.
    const auto path = dir / (job_id + "-" + std::to_string(index) + "." + ext);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return std::nullopt;

    std::ifstream is(path, std::ios::binary);
    if (!is) return std::nullopt;
    StoredImage out;
    out.bytes.assign(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
    if (out.bytes.empty()) return std::nullopt;
    out.mime = mime_for_ext(ext);
    return out;
}

namespace {

std::string random_job_id() {
    unsigned char b[12];
    if (RAND_bytes(b, sizeof(b)) != 1) {
        // Non-cryptographic fallback; ids only need to be unguessable-enough on
        // a single-tenant server, and RAND_bytes effectively never fails here.
        const auto n = std::chrono::steady_clock::now().time_since_epoch().count();
        for (size_t i = 0; i < sizeof(b); ++i) b[i] = static_cast<unsigned char>(n >> (i * 8));
    }
    static const char* hex = "0123456789abcdef";
    std::string s = "job_";
    for (unsigned char c : b) { s += hex[c >> 4]; s += hex[c & 0x0F]; }
    return s;
}

}  // namespace

JobStore::JobStore(int ttl_seconds, int max_concurrent)
    : ttl_(ttl_seconds > 0 ? ttl_seconds : 86400),
      max_concurrent_(max_concurrent > 0 ? max_concurrent : 4) {}

void JobStore::evict_locked() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->second->status != ImageJob::Status::Pending &&
            now - it->second->created > ttl_) {
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string JobStore::submit(const std::string& kind, const std::string& principal,
                             JobWork work) {
    auto job = std::make_shared<ImageJob>();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        evict_locked();
        job->id        = random_job_id();
        job->kind      = kind;
        job->principal = principal;
        job->created   = std::chrono::steady_clock::now();
        jobs_[job->id] = job;

        if (active_ >= max_concurrent_) {
            job->status = ImageJob::Status::Error;
            job->error  = "server is busy (too many concurrent renders); try again shortly";
            spdlog::warn("job {} rejected: concurrency cap {} reached", job->id, max_concurrent_);
            return job->id;
        }
        ++active_;
    }

    try {
        std::thread([this, job, work = std::move(work)]() mutable {
            JobOutput out;
            std::string err;
            try {
                out = work(job->id);
            } catch (const std::exception& e) {
                err = e.what();
            } catch (...) {
                err = "unknown error";
            }
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (err.empty()) {
                    job->status  = ImageJob::Status::Done;
                    job->images  = std::move(out.images);
                    job->caption = std::move(out.caption);
                    spdlog::info("job {} done ({} image(s))", job->id, job->images.size());
                } else {
                    job->status = ImageJob::Status::Error;
                    job->error  = err;
                    spdlog::warn("job {} failed: {}", job->id, err);
                }
                --active_;
            }
            cv_.notify_all();
        }).detach();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(mtx_);
        job->status = ImageJob::Status::Error;
        job->error  = std::string("could not start render thread: ") + e.what();
        --active_;
        cv_.notify_all();
    }

    return job->id;
}

std::optional<ImageJob> JobStore::wait_for(const std::string& id,
                                           std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lk(mtx_);
    evict_locked();
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return std::nullopt;
    auto job = it->second;  // shared_ptr keeps it alive across the wait
    if (job->status == ImageJob::Status::Pending) {
        cv_.wait_for(lk, wait, [&] { return job->status != ImageJob::Status::Pending; });
    }
    return *job;  // snapshot copy
}

std::optional<GeneratedImage> JobStore::get_image(const std::string& id, size_t index) {
    std::lock_guard<std::mutex> lk(mtx_);
    evict_locked();
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return std::nullopt;
    const auto& job = *it->second;
    if (job.status != ImageJob::Status::Done || index >= job.images.size()) {
        return std::nullopt;
    }
    return job.images[index];  // copy of {b64, mime}
}

}  // namespace gptimage
