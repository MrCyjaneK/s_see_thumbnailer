#include "thumbnailer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fstream>
#include <string>

static int wait_job(int (*progress)(long long, int), const char* (*progress_str)(long long),
                    void (*free_fn)(void*), long long job) {
  std::string last;
  for (;;) {
    int p = progress(job, 100);
    const char* msg = progress_str(job);
    std::string cur = msg ? msg : "";
    if (msg) free_fn((void*)msg);
    if (cur != last) {
      fprintf(stderr, "[thumbnailer] %3d%% %s\n", p, cur.c_str());
      fflush(stderr);
      last = cur;
    }
    if (p >= 100) return (last == "done") ? 0 : 1;
    usleep(100000);
  }
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <image-or-video> <output.jpg>\n", argv[0]);
    return 1;
  }
  if (SPACESEE_thumbnailer_load("/tmp/thumbnailer-work", "") != 0) {
    const char* e = SPACESEE_thumbnailer_loadError();
    fprintf(stderr, "load failed: %s\n", e ? e : "unknown");
    if (e) SPACESEE_thumbnailer_free((void*)e);
    return 1;
  }
  long long job = SPACESEE_thumbnailer_analyze(argv[1], argv[2], "");
  int rc = wait_job(SPACESEE_thumbnailer_progress, SPACESEE_thumbnailer_progressString,
                    SPACESEE_thumbnailer_free, job);
  if (rc != 0) {
    const char* msg = SPACESEE_thumbnailer_progressString(job);
    fprintf(stderr, "analyze failed: %s\n", msg ? msg : "unknown");
    if (msg) SPACESEE_thumbnailer_free((void*)msg);
    return 1;
  }
  std::ifstream in(argv[2], std::ios::binary);
  if (!in) {
    fprintf(stderr, "missing output %s\n", argv[2]);
    return 1;
  }
  char magic[3] = {};
  in.read(magic, 2);
  if (static_cast<unsigned char>(magic[0]) == 0xff && static_cast<unsigned char>(magic[1]) == 0xd8) {
    fprintf(stderr, "wrote jpeg %s\n", argv[2]);
  } else {
    in.clear();
    in.seekg(0);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    fprintf(stderr, "wrote error text (%zu bytes): %s\n", text.size(), text.c_str());
  }
  return 0;
}
