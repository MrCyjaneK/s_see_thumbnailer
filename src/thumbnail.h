#pragma once

#include <functional>
#include <string>

namespace thumbnailer {

using ProgressFn = std::function<void(int progress, const std::string& msg)>;

// Decode the picture (a HEIF tile grid is assembled first) or the first video frame,
// autorotate, fit inside 400px, write JPEG quality ~50.
// Returns true on success. On failure, err is set and nothing is written to output_path.
bool make_thumbnail(const std::string& input_path, const std::string& output_path, ProgressFn progress,
                    std::string& err);

bool write_text_file(const std::string& path, const std::string& text, std::string& err);

}  // namespace thumbnailer
