#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include <dr_mp3.h>
#include <dr_flac.h>
#include <dr_wav.h>

#define STB_VORBIS_NO_PUSHDATA_API
#include <stb_vorbis.c>
