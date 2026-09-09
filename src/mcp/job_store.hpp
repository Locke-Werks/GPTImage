#pragma once

#include <gptimage/image_client.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gptimage {

// The finished product of a render: the images plus the human-readable caption
// the tool would have returned synchronously.
struct JobOutput {
    std::vector<GeneratedImage> images;
    std::string                 caption;
};

// The work a job runs on its background thread. It is handed its own job id so
// a render can be written to disk under the same name its hosted URL uses.
// Throwing marks the job Error with the exception's message.
using JobWork = std::function<JobOutput(const std::string& job_id)>;

// ---------------------------------------------------------------------------
// Disk tier
// ---------------------------------------------------------------------------
//
// The JobStore holds a render in memory for job_ttl_seconds and then drops it,
// which is fine for delivering an image and wrong for keeping one. Writing the
// same bytes to a directory gives the render a life past that TTL: locally it
// puts the file somewhere the user actually has it, and on a hosted deployment
// it stops /i/<job_id> from 404ing a day later.

// Raw bytes of a render read back from disk, ready to serve.
struct StoredImage {
    std::vector<unsigned char> bytes;
    std::string                mime;
};

// Write each image of a finished render into `dir` as <job_id>-<index>.<ext>,
// the same name its hosted URL uses, so the HTTP route can serve it from disk
// with no index to consult. Creates `dir` if missing. Returns the paths written.
//
// Never throws: a render the caller has already paid for must not be lost
// because a disk was full or a path was unwritable. A failure is logged and the
// image is simply not saved.
std::vector<std::filesystem::path> save_render(
    const std::filesystem::path& dir, const std::string& job_id,
    const std::vector<GeneratedImage>& images, int max_files);

// Read one image of a previously saved render. `ext` comes from the request
// path. std::nullopt when nothing is stored under that name.
std::optional<StoredImage> load_render(
    const std::filesystem::path& dir, const std::string& job_id, size_t index,
    const std::string& ext);

struct ImageJob {
    enum class Status { Pending, Done, Error };
    std::string id;
    std::string kind;         // "generate" | "edit"
    std::string principal;
    Status      status = Status::Pending;
    std::vector<GeneratedImage> images;   // valid when Done
    std::string caption;                  // valid when Done
    std::string error;                    // valid when Error
    std::chrono::steady_clock::time_point created;
};

// Thread-safe async registry for image renders. submit() runs the work on a
// detached background thread and returns a job id immediately; wait_for() blocks
// up to a bounded window for the job to leave Pending, so a fast render is
// delivered on the first poll and a slow one is fetched in safe, sub-timeout
// chunks. Finished jobs are retained for `ttl_seconds` so the result tool can
// pick them up, then evicted.
class JobStore {
public:
    explicit JobStore(int ttl_seconds = 86400, int max_concurrent = 4);

    // Start `work`; returns the new job id. If the concurrency cap is already
    // reached the job is created already-Errored (no thread is spawned), so the
    // caller still gets an id whose result explains the rejection.
    std::string submit(const std::string& kind,
                       const std::string& principal,
                       JobWork work);

    // Wait up to `wait` for job `id` to leave Pending. Returns a snapshot copy,
    // or std::nullopt if the id is unknown or already evicted.
    std::optional<ImageJob> wait_for(const std::string& id,
                                     std::chrono::milliseconds wait);

    // Fetch one finished image for the HTTP image route. Returns the image at
    // `index` of a Done job, or std::nullopt if the id is unknown/evicted, the
    // job is not Done, or the index is out of range. Does not block.
    std::optional<GeneratedImage> get_image(const std::string& id, size_t index);

private:
    void evict_locked();  // drop expired finished jobs; caller holds mtx_

    std::mutex              mtx_;
    std::condition_variable cv_;
    std::unordered_map<std::string, std::shared_ptr<ImageJob>> jobs_;
    std::chrono::seconds    ttl_;
    int                     max_concurrent_;
    int                     active_ = 0;
};

}  // namespace gptimage
