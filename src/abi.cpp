#include "jobs.h"
#include "thumbnail.h"

#include "thumbnailer.h"

#include <cstring>
#include <string>
#include <filesystem>

namespace {

std::string load_error;

char* dup_cstr(const std::string& s) { return strdup(s.c_str()); }

}  // namespace

extern "C" int SPACESEE_thumbnailer_load(const char* workDir, const char* assetsDir) {
  (void)assetsDir;
  load_error.clear();
  if (!workDir || !workDir[0]) {
    load_error = "work dir is empty";
    return 1;
  }
  std::error_code ec;
  std::filesystem::create_directories(workDir, ec);
  if (ec) {
    load_error = ec.message();
    return 1;
  }
  return 0;
}

extern "C" const char* SPACESEE_thumbnailer_loadError(void) {
  if (load_error.empty()) return nullptr;
  return dup_cstr(load_error);
}

extern "C" void SPACESEE_thumbnailer_unload(void) {}

extern "C" int SPACESEE_thumbnailer_isHeavy(void) { return 0; }

extern "C" int SPACESEE_thumbnailer_assetCount(void) { return 0; }

extern "C" const char* SPACESEE_thumbnailer_asset(int index) {
  (void)index;
  return nullptr;
}

extern "C" const char* SPACESEE_thumbnailer_version(void) { return dup_cstr("1.0.0"); }

extern "C" const char* SPACESEE_thumbnailer_extension(void) { return dup_cstr("jpg"); }

extern "C" const char* SPACESEE_thumbnailer_filetypes(void) {
  return dup_cstr(
      ".jpg,.jpeg,.png,.webp,.gif,.bmp,.tiff,.tif,.heic,.heif,"
      ".mp4,.mov,.mkv,.webm,.avi,.m4v,.mpeg,.mpg,.3gp,.ts,.wmv,.flv,.hevc");
}

extern "C" long long SPACESEE_thumbnailer_maxInputSize(void) { return 16LL * 1024 * 1024 * 1024; }

namespace thumbnailer::jobs {
void run_analyze(JobId id, std::string input, std::string output) {
  auto progress = [id](int p, const std::string& msg) { update_progress(id, p, msg); };
  try {
    std::string err;
    if (!make_thumbnail(input, output, progress, err)) {
      if (err.empty()) err = "thumbnail failed";
      std::string write_err;
      if (!write_text_file(output, err, write_err)) {
        fail_job(id, write_err.empty() ? err : write_err);
        return;
      }
    }
    finish_job(id);
  } catch (const std::exception& ex) {
    std::string write_err;
    if (!write_text_file(output, ex.what(), write_err)) {
      fail_job(id, ex.what());
      return;
    }
    finish_job(id);
  }
}
}  // namespace thumbnailer::jobs

extern "C" long long SPACESEE_thumbnailer_analyze(const char* inputPath, const char* outputPath, const char* dataDir) {
  (void)dataDir;
  auto id = thumbnailer::jobs::new_job_id();
  thumbnailer::jobs::start_analyze(id, inputPath ? inputPath : "", outputPath ? outputPath : "");
  return static_cast<long long>(id);
}

extern "C" int SPACESEE_thumbnailer_progress(long long jobId, int scale) {
  return thumbnailer::jobs::progress(jobId, scale);
}

extern "C" const char* SPACESEE_thumbnailer_progressString(long long jobId) {
  return dup_cstr(thumbnailer::jobs::progress_string(jobId));
}

extern "C" void SPACESEE_thumbnailer_free(void* ptr) { free(ptr); }
