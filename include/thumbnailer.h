#ifndef SPACESEE_THUMBNAILER_H
#define SPACESEE_THUMBNAILER_H

#ifdef __cplusplus
extern "C" {
#endif

int SPACESEE_thumbnailer_load(const char *workDir, const char *assetsDir);
const char *SPACESEE_thumbnailer_loadError(void);
void SPACESEE_thumbnailer_unload(void);
int SPACESEE_thumbnailer_isHeavy(void);
int SPACESEE_thumbnailer_assetCount(void);
const char *SPACESEE_thumbnailer_asset(int index);
const char *SPACESEE_thumbnailer_version(void);
const char *SPACESEE_thumbnailer_extension(void);
const char *SPACESEE_thumbnailer_filetypes(void);
long long SPACESEE_thumbnailer_maxInputSize(void);
long long SPACESEE_thumbnailer_analyze(const char *inputPath, const char *outputPath, const char *dataDir);
int SPACESEE_thumbnailer_progress(long long jobId, int scale);
const char *SPACESEE_thumbnailer_progressString(long long jobId);
void SPACESEE_thumbnailer_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
