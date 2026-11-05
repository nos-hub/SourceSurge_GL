// photo_shared.h - photo camera constants + wire encoding, shared by dlls/ (server) and cl_dll/ (client)
//
// A photo is PHOTO_W x PHOTO_H pixels, 1 byte per pixel (RGB332), sent in PHOTO_CHUNKS chunks of
// PHOTO_CHUNK_BYTES bytes.
//   client -> server : "ss_photo <id> <chunk> <text>"   (text = PhotoEncodeChunk, PHOTO_CHUNK_CHARS chars)
//                      "ss_photo <id> fail"             (capture failed, give the default spray back)
//   server -> clients: user messages PhotoCap / PhotoDat / PhotoSpr (see photo.cpp)
#pragma once

constexpr int PHOTO_W = 64;
constexpr int PHOTO_H = 64;
constexpr int PHOTO_BYTES = PHOTO_W * PHOTO_H;
constexpr int PHOTO_CHUNK_BYTES = 128;
constexpr int PHOTO_CHUNKS = PHOTO_BYTES / PHOTO_CHUNK_BYTES; // 32
static_assert(PHOTO_BYTES % PHOTO_CHUNK_BYTES == 0, "photo size must be a whole number of chunks");

// 128 bytes -> 43 groups of 3 bytes (last group padded with one zero byte) -> 172 chars
constexpr int PHOTO_CHUNK_GROUPS = (PHOTO_CHUNK_BYTES + 2) / 3;
constexpr int PHOTO_CHUNK_CHARS = PHOTO_CHUNK_GROUPS * 4;

// Alphabet avoids '/' (two of them start a comment in the console tokenizer), quotes, ';' and spaces.
inline const char* PhotoAlphabet()
{
	return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
}

// out must hold PHOTO_CHUNK_CHARS + 1 chars
inline void PhotoEncodeChunk(const unsigned char* src, char* out)
{
	const char* a = PhotoAlphabet();
	for (int g = 0; g < PHOTO_CHUNK_GROUPS; g++)
	{
		unsigned char b[3] = {0, 0, 0};
		for (int i = 0; i < 3; i++)
		{
			const int idx = g * 3 + i;
			if (idx < PHOTO_CHUNK_BYTES)
				b[i] = src[idx];
		}
		const unsigned v = (unsigned(b[0]) << 16) | (unsigned(b[1]) << 8) | b[2];
		out[g * 4 + 0] = a[(v >> 18) & 63];
		out[g * 4 + 1] = a[(v >> 12) & 63];
		out[g * 4 + 2] = a[(v >> 6) & 63];
		out[g * 4 + 3] = a[v & 63];
	}
	out[PHOTO_CHUNK_CHARS] = 0;
}

// in must be exactly PHOTO_CHUNK_CHARS chars; dst receives PHOTO_CHUNK_BYTES bytes. false = bad input.
inline bool PhotoDecodeChunk(const char* in, unsigned char* dst)
{
	signed char rev[256];
	for (int i = 0; i < 256; i++)
		rev[i] = -1;
	const char* a = PhotoAlphabet();
	for (int i = 0; i < 64; i++)
		rev[(unsigned char)a[i]] = (signed char)i;

	for (int i = 0; i < PHOTO_CHUNK_CHARS; i++)
	{
		if (in[i] == 0)
			return false;
		if (rev[(unsigned char)in[i]] < 0)
			return false;
	}
	if (in[PHOTO_CHUNK_CHARS] != 0)
		return false;

	for (int g = 0; g < PHOTO_CHUNK_GROUPS; g++)
	{
		const unsigned v = (unsigned(rev[(unsigned char)in[g * 4 + 0]]) << 18) | (unsigned(rev[(unsigned char)in[g * 4 + 1]]) << 12) | (unsigned(rev[(unsigned char)in[g * 4 + 2]]) << 6) | unsigned(rev[(unsigned char)in[g * 4 + 3]]);
		const unsigned char b[3] = {(unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v};
		for (int i = 0; i < 3; i++)
		{
			const int idx = g * 3 + i;
			if (idx < PHOTO_CHUNK_BYTES)
				dst[idx] = b[i];
		}
	}
	return true;
}
