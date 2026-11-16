/* Capstone Disassembly Engine */
/* By Nguyen Anh Quynh <aquynh@gmail.com>, 2013-2015 */

#ifndef CS_SSTREAM_H_
#define CS_SSTREAM_H_

#include "include/capstone/platform.h"

typedef struct SStream {
	char buffer[512];
	int index;
} SStream;

#ifdef CAPSTONE_DIET
#define SStream_Init(ss)
#define SStream_concat0(ss, s)
#define SStream_concat(ss, fmt...)
#define printInt64Bang(ss, val)
#define printUInt64Bang(ss, val)
#define printInt64(ss, val)
#define printInt32Bang(ss, val)
#define printInt32(ss, val)
#define printUInt32Bang(ss, val)
#define printUInt32(ss, val)

// print number in decimal mode
void printInt32BangDec(SStream *O, int32_t val);
#else
void SStream_Init(SStream *ss);

void SStream_concat(SStream *ss, const char *fmt, ...);

void SStream_concat0(SStream *ss, const char *s);

void printInt64Bang(SStream *O, int64_t val);

void printUInt64Bang(SStream *O, uint64_t val);

void printInt64(SStream *O, int64_t val);

void printInt32Bang(SStream *O, int32_t val);

void printInt32(SStream *O, int32_t val);

void printUInt32Bang(SStream *O, uint32_t val);

void printUInt32(SStream *O, uint32_t val);

// print number in decimal mode
void printInt32BangDec(SStream *O, int32_t val);
#endif

#endif
