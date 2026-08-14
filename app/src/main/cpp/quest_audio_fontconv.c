#include "audio/rom/lib/audio.h"

#ifdef PORT

/*
 * Base-cart F-Zero X soundfont conversion for 64-bit hosts.
 *
 * The ROM image stores every internal reference as a big-endian u32 offset from the
 * start of the font.  The original N64 relocation patches those offsets in-place into
 * pointers and then reads Drum/Instrument/Sample structs directly from the blob.  That
 * cannot work on AArch64: native pointers are 8 bytes and host struct layouts expand.
 *
 * G-Diffuser's Expansion-Kit audio path already solves the same problem by parsing the
 * raw image into host-native objects.  This is the base-cart equivalent (its font header
 * has only a drum offset followed by instrument offsets; there is no SFX array slot).
 */

#define GDX_FONTCONV_MAX_OBJS 256
#define GDX_FONTCONV_ENV_POINTS 64

typedef struct {
    u32 offset;
    void* host;
} GdxFontConvEntry;

typedef struct {
    const u8* data;
    SampleBankRelocInfo* reloc;
    s32 fontId;
    GdxFontConvEntry samples[GDX_FONTCONV_MAX_OBJS];
    s32 numSamples;
    GdxFontConvEntry loops[GDX_FONTCONV_MAX_OBJS];
    s32 numLoops;
    GdxFontConvEntry books[GDX_FONTCONV_MAX_OBJS];
    s32 numBooks;
    GdxFontConvEntry envs[GDX_FONTCONV_MAX_OBJS];
    s32 numEnvs;
} GdxFontConv;

extern void* gdx_rdram_persist_alloc_raw(size_t size, size_t align);
extern void gdx_dbg_logf(const char* fmt, ...);

static void gdx_fontconv_zero(void* ptr, size_t size) {
    u8* p = (u8*)ptr;
    size_t i;
    for (i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

static void* gdx_fontconv_alloc(size_t size) {
    void* p = gdx_rdram_persist_alloc_raw(size, 16u);
    if (p != NULL) {
        gdx_fontconv_zero(p, size);
    }
    return p;
}

static u32 gdx_rd_u32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static s16 gdx_rd_s16(const u8* p) {
    return (s16)(((u16)p[0] << 8) | (u16)p[1]);
}

static f32 gdx_rd_f32(const u8* p) {
    union {
        u32 w;
        f32 f;
    } u;
    u.w = gdx_rd_u32(p);
    return u.f;
}

static void* gdx_fontconv_find(GdxFontConvEntry* list, s32 count, u32 offset) {
    s32 i;
    for (i = 0; i < count; i++) {
        if (list[i].offset == offset) {
            return list[i].host;
        }
    }
    return NULL;
}

static void gdx_fontconv_remember(GdxFontConvEntry* list, s32* count, u32 offset, void* host) {
    if (*count < GDX_FONTCONV_MAX_OBJS) {
        list[*count].offset = offset;
        list[*count].host = host;
        (*count)++;
    }
}

static EnvelopePoint* gdx_fontconv_envelope(GdxFontConv* conv, u32 offset) {
    EnvelopePoint* env;
    s32 i;

    if (offset == 0) {
        return NULL;
    }
    env = gdx_fontconv_find(conv->envs, conv->numEnvs, offset);
    if (env != NULL) {
        return env;
    }
    env = gdx_fontconv_alloc(GDX_FONTCONV_ENV_POINTS * sizeof(EnvelopePoint));
    if (env == NULL) {
        return NULL;
    }
    for (i = 0; i < GDX_FONTCONV_ENV_POINTS; i++) {
        env[i].delay = gdx_rd_s16(conv->data + offset + i * 4);
        env[i].arg = gdx_rd_s16(conv->data + offset + i * 4 + 2);
        if (env[i].delay <= 0) {
            break;
        }
    }
    gdx_fontconv_remember(conv->envs, &conv->numEnvs, offset, env);
    return env;
}

static AdpcmLoop* gdx_fontconv_loop(GdxFontConv* conv, u32 offset) {
    AdpcmLoop* loop;
    s32 i;

    if (offset == 0) {
        return NULL;
    }
    loop = gdx_fontconv_find(conv->loops, conv->numLoops, offset);
    if (loop != NULL) {
        return loop;
    }
    loop = gdx_fontconv_alloc(sizeof(AdpcmLoop));
    if (loop == NULL) {
        return NULL;
    }
    loop->header.start = gdx_rd_u32(conv->data + offset);
    loop->header.end = gdx_rd_u32(conv->data + offset + 4);
    loop->header.count = gdx_rd_u32(conv->data + offset + 8);
    if (loop->header.count != 0) {
        for (i = 0; i < 16; i++) {
            loop->predictorState[i] = gdx_rd_s16(conv->data + offset + 0x10 + i * 2);
        }
    }
    gdx_fontconv_remember(conv->loops, &conv->numLoops, offset, loop);
    return loop;
}

static AdpcmBook* gdx_fontconv_book(GdxFontConv* conv, u32 offset) {
    AdpcmBook* book;
    s32 order;
    s32 numPredictors;
    s32 numEntries;
    s32 i;

    if (offset == 0) {
        return NULL;
    }
    book = gdx_fontconv_find(conv->books, conv->numBooks, offset);
    if (book != NULL) {
        return book;
    }
    order = (s32)gdx_rd_u32(conv->data + offset);
    numPredictors = (s32)gdx_rd_u32(conv->data + offset + 4);
    if ((order < 1) || (order > 8) || (numPredictors < 1) || (numPredictors > 8)) {
        gdx_dbg_logf("[fontconv-rom] invalid book font=%d off=%x order=%d predictors=%d\n",
                     conv->fontId, (unsigned)offset, order, numPredictors);
        return NULL;
    }
    numEntries = 8 * order * numPredictors;
    book = gdx_fontconv_alloc(sizeof(AdpcmBookHeader) + numEntries * sizeof(s16));
    if (book == NULL) {
        return NULL;
    }
    book->header.order = order;
    book->header.numPredictors = numPredictors;
    for (i = 0; i < numEntries; i++) {
        book->book[i] = gdx_rd_s16(conv->data + offset + 8 + i * 2);
    }
    gdx_fontconv_remember(conv->books, &conv->numBooks, offset, book);
    return book;
}

static Sample* gdx_fontconv_sample(GdxFontConv* conv, u32 offset) {
    Sample* sample;
    u32 flags;
    u32 rawAddr;
    u32 origMedium;
    uintptr_t base;

    if (offset == 0) {
        return NULL;
    }
    sample = gdx_fontconv_find(conv->samples, conv->numSamples, offset);
    if (sample != NULL) {
        return sample;
    }
    sample = gdx_fontconv_alloc(sizeof(Sample));
    if (sample == NULL) {
        return NULL;
    }

    flags = gdx_rd_u32(conv->data + offset);
    sample->codec = (flags >> 28) & 0xF;
    origMedium = (flags >> 26) & 0x3;
    sample->unk_bit26 = (flags >> 25) & 0x1;
    sample->size = flags & 0xFFFFFF;
    rawAddr = gdx_rd_u32(conv->data + offset + 4);

    switch (origMedium) {
        case MEDIUM_RAM:
            base = (uintptr_t)(u32)conv->reloc->baseAddr1;
            sample->sampleAddr = (u8*)(base + rawAddr);
            sample->medium = conv->reloc->medium1;
            break;
        case MEDIUM_LBA:
            base = (uintptr_t)(u32)conv->reloc->baseAddr2;
            sample->sampleAddr = (u8*)(base + rawAddr);
            sample->medium = conv->reloc->medium2;
            break;
        default:
            sample->sampleAddr = (u8*)(uintptr_t)rawAddr;
            sample->medium = origMedium;
            break;
    }

    sample->loop = gdx_fontconv_loop(conv, gdx_rd_u32(conv->data + offset + 8));
    sample->book = gdx_fontconv_book(conv, gdx_rd_u32(conv->data + offset + 0xC));
    sample->isRelocated = 1;

    if (sample->unk_bit26 && (sample->medium != MEDIUM_RAM) && gNumUsedSamples < 128) {
        gUsedSamples[gNumUsedSamples++] = sample;
    }

    gdx_fontconv_remember(conv->samples, &conv->numSamples, offset, sample);
    return sample;
}

static void gdx_fontconv_tuned_sample(GdxFontConv* conv, TunedSample* dest, u32 entryOffset) {
    dest->sample = gdx_fontconv_sample(conv, gdx_rd_u32(conv->data + entryOffset));
    dest->tuning = gdx_rd_f32(conv->data + entryOffset + 4);
}

void AudioLoad_RelocateFont(s32 fontId, uintptr_t fontBaseAddr, void* relocData) {
    static GdxFontConv conv;
    const u8* fontData = (const u8*)fontBaseAddr;
    SampleBankRelocInfo* reloc = (SampleBankRelocInfo*)relocData;
    s32 numDrums = gSoundFontList[fontId].numDrums;
    s32 numInstruments = gSoundFontList[fontId].numInstruments;
    u32 drumBaseOffset;
    Drum** drumPtrs = NULL;
    Instrument** instPtrs = NULL;
    s32 i;

    gdx_fontconv_zero(&conv, sizeof(conv));
    conv.data = fontData;
    conv.reloc = reloc;
    conv.fontId = fontId;

    drumBaseOffset = gdx_rd_u32(fontData);

    if (numDrums > 0) {
        drumPtrs = gdx_fontconv_alloc(numDrums * sizeof(Drum*));
        if (drumPtrs != NULL && drumBaseOffset != 0) {
            for (i = 0; i < numDrums; i++) {
                u32 drumOffset = gdx_rd_u32(fontData + drumBaseOffset + i * 4);
                if (drumOffset != 0) {
                    Drum* drum = gdx_fontconv_alloc(sizeof(Drum));
                    if (drum == NULL) {
                        continue;
                    }
                    drum->adsrDecayIndex = fontData[drumOffset];
                    drum->pan = fontData[drumOffset + 1];
                    drum->isRelocated = 1;
                    gdx_fontconv_tuned_sample(&conv, &drum->tunedSample, drumOffset + 4);
                    drum->envelope = gdx_fontconv_envelope(&conv, gdx_rd_u32(fontData + drumOffset + 0xC));
                    drumPtrs[i] = drum;
                }
            }
        }
    }

    if (numInstruments > 126) {
        numInstruments = 126;
    }
    if (numInstruments > 0) {
        instPtrs = gdx_fontconv_alloc(numInstruments * sizeof(Instrument*));
        if (instPtrs != NULL) {
            for (i = 0; i < numInstruments; i++) {
                /* Base-cart layout: [drumArrayOffset][instrument0Offset]... */
                u32 instOffset = gdx_rd_u32(fontData + 4 + i * 4);
                if (instOffset != 0) {
                    Instrument* inst = gdx_fontconv_alloc(sizeof(Instrument));
                    if (inst == NULL) {
                        continue;
                    }
                    inst->isRelocated = 1;
                    inst->normalRangeLo = fontData[instOffset + 1];
                    inst->normalRangeHi = fontData[instOffset + 2];
                    inst->adsrDecayIndex = fontData[instOffset + 3];
                    inst->envelope = gdx_fontconv_envelope(&conv, gdx_rd_u32(fontData + instOffset + 4));
                    if (inst->normalRangeLo != 0) {
                        gdx_fontconv_tuned_sample(&conv, &inst->lowPitchTunedSample, instOffset + 8);
                    }
                    gdx_fontconv_tuned_sample(&conv, &inst->normalPitchTunedSample, instOffset + 0x10);
                    if (inst->normalRangeHi != 0x7F) {
                        gdx_fontconv_tuned_sample(&conv, &inst->highPitchTunedSample, instOffset + 0x18);
                    }
                    instPtrs[i] = inst;
                }
            }
        }
    }

    gSoundFontList[fontId].drums = drumPtrs;
    gSoundFontList[fontId].instruments = instPtrs;

    gdx_dbg_logf("[fontconv-rom] font=%d converted drums=%d instruments=%d usedSamples=%d base=%p\n",
                 fontId, numDrums, numInstruments, gNumUsedSamples, fontData);
}

#endif /* PORT */
