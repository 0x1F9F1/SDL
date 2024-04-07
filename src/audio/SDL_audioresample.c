/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#include "SDL_sysaudio.h"
#include "SDL_audioresample.h"

// SDL's resampler uses a "bandlimited interpolation" algorithm:
//     https://ccrma.stanford.edu/~jos/resample/

// TODO: Support changing this at runtime
#define RESAMPLER_ZERO_CROSSINGS 5

// For a given srcpos, `srcpos + frame` are sampled, where `-RESAMPLER_ZERO_CROSSINGS < frame <= RESAMPLER_ZERO_CROSSINGS`.
// Note, when upsampling, it is also possible to start sampling from `srcpos = -1`.
#define RESAMPLER_MAX_PADDING_FRAMES (RESAMPLER_ZERO_CROSSINGS + 1)
#define RESAMPLER_SAMPLES_PER_FRAME (RESAMPLER_ZERO_CROSSINGS * 2)

#define RESAMPLER_BITS_PER_SAMPLE 16
#define RESAMPLER_BITS_PER_ZERO_CROSSING  ((RESAMPLER_BITS_PER_SAMPLE / 2) + 1)
#define RESAMPLER_SAMPLES_PER_ZERO_CROSSING  (1 << RESAMPLER_BITS_PER_ZERO_CROSSING)
#define RESAMPLER_FILTER_INTERP_BITS  (32 - RESAMPLER_BITS_PER_ZERO_CROSSING)
#define RESAMPLER_FILTER_INTERP_RANGE (1 << RESAMPLER_FILTER_INTERP_BITS)

static void ResampleFrame_Generic(const float *src, float *dst, const float *filter, float interp, int chans)
{
    int i, chan;

    float scales[RESAMPLER_SAMPLES_PER_FRAME];

    // Interpolate between the nearest two filters
    for (i = 0; i < RESAMPLER_SAMPLES_PER_FRAME; i++) {
        scales[i] = (filter[i] * (1.0f - interp)) + (filter[i + RESAMPLER_SAMPLES_PER_FRAME] * interp);
    }

    for (chan = 0; chan < chans; chan++) {
        float out = 0.0f;

        for (i = 0; i < RESAMPLER_SAMPLES_PER_FRAME; i++) {
            out += src[i * chans + chan] * scales[i];
        }

        dst[chan] = out;
    }
}

static void ResampleFrame_Mono(const float *src, float *dst, const float *filter, float interp, int chans)
{
    int i;
    float out = 0.0f;

    for (i = 0; i < RESAMPLER_SAMPLES_PER_FRAME; i++) {
        // Interpolate between the nearest two filters
        const float scale = (filter[i] * (1.0f - interp)) + (filter[i + RESAMPLER_SAMPLES_PER_FRAME] * interp);

        out += src[i] * scale;
    }

    dst[0] = out;
}

static void ResampleFrame_Stereo(const float *src, float *dst, const float *filter, float interp, int chans)
{
    int i;
    float out0 = 0.0f;
    float out1 = 0.0f;

    for (i = 0; i < RESAMPLER_SAMPLES_PER_FRAME; i++) {
        // Interpolate between the nearest two filters
        const float scale = (filter[i] * (1.0f - interp)) + (filter[i + RESAMPLER_SAMPLES_PER_FRAME] * interp);

        out0 += src[i * 2 + 0] * scale;
        out1 += src[i * 2 + 1] * scale;
    }

    dst[0] = out0;
    dst[1] = out1;
}

#ifdef SDL_SSE_INTRINSICS
static void SDL_TARGETING("sse") ResampleFrame_Generic_SSE(const float *src, float *dst, const float *filter, float interp, int chans)
{
#if RESAMPLER_SAMPLES_PER_FRAME != 10
#error Invalid samples per frame
#endif

    // Load the filter
    __m128 f0 = _mm_loadu_ps(filter + 0);
    __m128 f1 = _mm_loadu_ps(filter + 4);
    __m128 f2 = _mm_loadl_pi(_mm_setzero_ps(), (const __m64 *)(filter + 8));

    __m128 g0 = _mm_loadu_ps(filter + 10);
    __m128 g1 = _mm_loadu_ps(filter + 14);
    __m128 g2 = _mm_loadl_pi(_mm_setzero_ps(), (const __m64 *)(filter + 18));

    __m128 interp1 = _mm_set1_ps(interp);
    __m128 interp2 = _mm_sub_ps(_mm_set1_ps(1.0f), interp1);

    // Linear interpolate the filter
    f0 = _mm_add_ps(_mm_mul_ps(f0, interp2), _mm_mul_ps(g0, interp1));
    f1 = _mm_add_ps(_mm_mul_ps(f1, interp2), _mm_mul_ps(g1, interp1));
    f2 = _mm_add_ps(_mm_mul_ps(f2, interp2), _mm_mul_ps(g2, interp1));

    if (chans == 2) {
        // Duplicate each of the filter elements and multiply by the input
        __m128 out =          _mm_mul_ps(_mm_loadu_ps(src + 0),  _mm_unpacklo_ps(f0, f0));
        out = _mm_add_ps(out, _mm_mul_ps(_mm_loadu_ps(src + 4),  _mm_unpackhi_ps(f0, f0)));
        out = _mm_add_ps(out, _mm_mul_ps(_mm_loadu_ps(src + 8),  _mm_unpacklo_ps(f1, f1)));
        out = _mm_add_ps(out, _mm_mul_ps(_mm_loadu_ps(src + 12), _mm_unpackhi_ps(f1, f1)));
        out = _mm_add_ps(out, _mm_mul_ps(_mm_loadu_ps(src + 16), _mm_unpacklo_ps(f2, f2)));

        // Add the lower and upper pairs together
        out = _mm_add_ps(out, _mm_movehl_ps(out, out));

        // Store the result
        _mm_storel_pi((__m64 *)dst, out);
        return;
    }

    if (chans == 1) {
        // Multiply the filter by the input
        __m128 out =          _mm_mul_ps(f0, _mm_loadu_ps(src + 0));
        out = _mm_add_ps(out, _mm_mul_ps(f1, _mm_loadu_ps(src + 4)));
        out = _mm_add_ps(out, _mm_mul_ps(f2, _mm_loadl_pi(_mm_setzero_ps(), (const __m64 *)(src + 8))));

        // Horizontal sum
        __m128 shuf = _mm_shuffle_ps(out, out, _MM_SHUFFLE(2, 3, 0, 1));
        out = _mm_add_ps(out, shuf);                  
        out = _mm_add_ss(out, _mm_movehl_ps(shuf, out));

        _mm_store_ss(dst, out);
        return;
    }

    int chan = 0;

    // Process 4 channels at once
    for (; chan + 4 <= chans; chan += 4) {
        const float* in = &src[chan];
        __m128 out = _mm_setzero_ps();

#define X(a, b, c) out = _mm_add_ps(out,             \
            _mm_mul_ps(_mm_loadu_ps(&in[a * chans]), \
            _mm_shuffle_ps(b, b, _MM_SHUFFLE(c, c, c, c))))

        X(0, f0, 0); X(1, f0, 1); X(2, f0, 2); X(3, f0, 3);
        X(4, f1, 0); X(5, f1, 1); X(6, f1, 2); X(7, f1, 3);
        X(8, f2, 0); X(9, f2, 1);

#undef X

        _mm_storeu_ps(&dst[chan], out);
    }

    // Process the remaining channels one at a time.
    // Channel counts 1,2,4,8 are already handled above, leaving 3,5,6,7 to deal with (looping 3,1,2,3 times).
    // Without vgatherdps (AVX2), this gets quite messy.
    // Since there's only 4 cases, just use a switch to compute the gather offsets at compile time.
    // Yes it's branchy, but should be easy for the branch predictor to deal with.
    for (; chan < chans; chan++) {
        const float* in = &src[chan];
        __m128 v0, v1, v2;

        switch (chans) {

#define X(a, b) _mm_unpacklo_ps(_mm_load_ss(&in[a]), _mm_load_ss(&in[b]))
#define Y(a, b, c, d) _mm_movelh_ps(X(a, b), X(c, d))
#define Z(n)                                \
    case n:                                 \
        v0 = Y(n * 0, n * 1, n * 2, n * 3); \
        v1 = Y(n * 4, n * 5, n * 6, n * 7); \
        v2 = X(n * 8, n * 9);               \
        break

            Z(3);
            Z(5);
            Z(6);
            Z(7);

#undef X
#undef Y
#undef Z

            default: return;
        }

        __m128 out = _mm_mul_ps(f0, v0);
        out = _mm_add_ps(out, _mm_mul_ps(f1, v1));
        out = _mm_add_ps(out, _mm_mul_ps(f2, v2));

        // Horizontal sum
        __m128 shuf = _mm_shuffle_ps(out, out, _MM_SHUFFLE(2, 3, 0, 1));
        out = _mm_add_ps(out, shuf);                  
        out = _mm_add_ss(out, _mm_movehl_ps(shuf, out));

        _mm_store_ss(&dst[chan], out);
    }
}
#endif

typedef void (*ResampleFrameFunc)(const float *src, float *dst, const float *filter, float interp, int chans);

static ResampleFrameFunc ResampleFrame[8];

#define RESAMPLER_FILTER_SIZE (RESAMPLER_SAMPLES_PER_FRAME * (RESAMPLER_SAMPLES_PER_ZERO_CROSSING + 1))

static float ResamplerFilter[RESAMPLER_FILTER_SIZE];

// This is a "modified" bessel function, so you can't use POSIX j0() */
// See https://mathworld.wolfram.com/ModifiedBesselFunctionoftheFirstKind.html 
static float bessel(float x)
{
    const float epsilon = 1e-12f;

    float sum = 0.0f;
    float i = 1.0f;
    float t = 1.0f;
    x *= x * 0.25f;

    while (t > epsilon) {
        sum += t;
        t *= x / (i * i);
        ++i;
    }

    return sum;
}

static void CubicCoef(float *interp, float frac)
{
    float frac2 = frac * frac;
    float frac3 = frac * frac2;

    interp[3] = -0.1666666667f * frac + 0.1666666667f * frac3;
    interp[2] = frac + 0.5f * frac2 - 0.5f * frac3;
    interp[0] = -0.3333333333f * frac + 0.5f * frac2 - 0.1666666667f * frac3;
    interp[1] = 1.0f - interp[3] - interp[2] - interp[0];
}

static float CubicInterp(float* interp, float* data)
{
    return data[0] * interp[0] + data[1] * interp[1] + data[2] * interp[2] + data[3] * interp[3];
}

static void GenerateKaiserTable(float beta, float* table, int tablelen)
{
    const float bessel_beta = bessel(beta);

    int i;

    for (i = 0; i <= tablelen; ++i) {
        table[i + 1] = bessel(beta * SDL_sqrtf(1.0f - (i * i / (float)(tablelen * tablelen)))) / bessel_beta;
    }

    table[0] = table[2];
    table[tablelen + 2] = 0.0f;
    table[tablelen + 3] = 0.0f;
}

// If KAISER_TABLE_SIZE is a multiple of RESAMPLER_ZERO_CROSSINGS,
// we can avoid recomputing the interp factors between each zero crossing
#define KAISER_TABLE_SIZE (RESAMPLER_ZERO_CROSSINGS * 4)

static void GenerateResamplerFilter()
{
    // Build a table combining the left and right wings, for faster access

    // if dB > 50, beta=(0.1102 * (dB - 8.7)), according to Matlab.
    const float dB = 80.0f;
    const float beta = 0.1102f * (dB - 8.7f);

    const int winglen = RESAMPLER_SAMPLES_PER_ZERO_CROSSING * RESAMPLER_ZERO_CROSSINGS;
    const float sinc_scale = SDL_PI_F / RESAMPLER_SAMPLES_PER_ZERO_CROSSING;

    // Generate a small kaiser table, which will we then use interpolate over.
    float kaiser[KAISER_TABLE_SIZE + 4];
    GenerateKaiserTable(beta, kaiser, KAISER_TABLE_SIZE);

    int i, j;

    for (i = 0; i < RESAMPLER_SAMPLES_PER_ZERO_CROSSING; ++i) {
        float s = SDL_sinf(i * sinc_scale) / sinc_scale;

        // The fractional part of the interpolation will say the same.
        float interp[4];
        CubicCoef(interp, (float)((i * KAISER_TABLE_SIZE) % winglen) / winglen);

        for (j = 0; j < RESAMPLER_ZERO_CROSSINGS; j++) {
            int n = j * RESAMPLER_SAMPLES_PER_ZERO_CROSSING + i;
            float v = 1.0f;

            if (n) {
                v = CubicInterp(interp, &kaiser[(n * KAISER_TABLE_SIZE) / winglen]) * s / n;
            }

            int lwing = (i * RESAMPLER_SAMPLES_PER_FRAME) + (RESAMPLER_ZERO_CROSSINGS - 1) - j;
            int rwing = (RESAMPLER_FILTER_SIZE - 1) - lwing;

            ResamplerFilter[lwing] = v;
            ResamplerFilter[rwing] = v;

            s = -s;
        }
    }

    for (i = 0; i < RESAMPLER_ZERO_CROSSINGS; ++i) {
        int rwing = i + RESAMPLER_ZERO_CROSSINGS;
        int lwing = (RESAMPLER_FILTER_SIZE - 1) - rwing;

        ResamplerFilter[lwing] = 0.0f;
        ResamplerFilter[rwing] = 0.0f;
    }
}

void SDL_SetupAudioResampler(void)
{
    static SDL_bool setup = SDL_FALSE;
    if (setup) {
        return;
    }

    GenerateResamplerFilter();

    int i;

    for (i = 0; i < 8; ++i) {
        ResampleFrame[i] = ResampleFrame_Generic;
    }

    ResampleFrame[0] = ResampleFrame_Mono;
    ResampleFrame[1] = ResampleFrame_Stereo;

#ifdef SDL_SSE_INTRINSICS
    if (SDL_HasSSE()) {
        for (i = 0; i < 8; ++i) {
            ResampleFrame[i] = ResampleFrame_Generic_SSE;
        }
    }
#endif

    setup = SDL_TRUE;
}

Sint64 SDL_GetResampleRate(int src_rate, int dst_rate)
{
    SDL_assert(src_rate > 0);
    SDL_assert(dst_rate > 0);

    Sint64 sample_rate = ((Sint64)src_rate << 32) / (Sint64)dst_rate;
    SDL_assert(sample_rate > 0);

    return sample_rate;
}

int SDL_GetResamplerHistoryFrames(void)
{
    // Even if we aren't currently resampling, make sure to keep enough history in case we need to later.

    return RESAMPLER_MAX_PADDING_FRAMES;
}

int SDL_GetResamplerPaddingFrames(Sint64 resample_rate)
{
    // This must always be <= SDL_GetResamplerHistoryFrames()

    return resample_rate ? RESAMPLER_MAX_PADDING_FRAMES : 0;
}

// These are not general purpose. They do not check for all possible underflow/overflow
SDL_FORCE_INLINE Sint64 ResamplerAdd(Sint64 a, Sint64 b, Sint64 *ret)
{
    if ((b > 0) && (a > SDL_MAX_SINT64 - b)) {
        return -1;
    }

    *ret = a + b;
    return 0;
}

SDL_FORCE_INLINE Sint64 ResamplerMul(Sint64 a, Sint64 b, Sint64 *ret)
{
    if ((b > 0) && (a > SDL_MAX_SINT64 / b)) {
        return -1;
    }

    *ret = a * b;
    return 0;
}

Sint64 SDL_GetResamplerInputFrames(Sint64 output_frames, Sint64 resample_rate, Sint64 resample_offset)
{
    // Calculate the index of the last input frame, then add 1.
    // ((((output_frames - 1) * resample_rate) + resample_offset) >> 32) + 1

    Sint64 output_offset;
    if (ResamplerMul(output_frames, resample_rate, &output_offset) ||
        ResamplerAdd(output_offset, -resample_rate + resample_offset + 0x100000000, &output_offset)) {
        output_offset = SDL_MAX_SINT64;
    }

    Sint64 input_frames = (Sint64)(Sint32)(output_offset >> 32);
    input_frames = SDL_max(input_frames, 0);

    return input_frames;
}

Sint64 SDL_GetResamplerOutputFrames(Sint64 input_frames, Sint64 resample_rate, Sint64 *inout_resample_offset)
{
    Sint64 resample_offset = *inout_resample_offset;

    // input_offset = (input_frames << 32) - resample_offset;
    Sint64 input_offset;
    if (ResamplerMul(input_frames, 0x100000000, &input_offset) ||
        ResamplerAdd(input_offset, -resample_offset, &input_offset)) {
        input_offset = SDL_MAX_SINT64;
    }

    // output_frames = div_ceil(input_offset, resample_rate)
    Sint64 output_frames = (input_offset > 0) ? (((input_offset - 1) / resample_rate) + 1) : 0;

    *inout_resample_offset = (output_frames * resample_rate) - input_offset;

    return output_frames;
}

void SDL_ResampleAudio(int chans, const float *src, int inframes, float *dst, int outframes,
                       Sint64 resample_rate, Sint64 *inout_resample_offset)
{
    int i;
    Sint64 srcpos = *inout_resample_offset;
    ResampleFrameFunc resample_frame = ResampleFrame[chans - 1];

    SDL_assert(resample_rate > 0);

    src -= (RESAMPLER_ZERO_CROSSINGS - 1) * chans;

    for (i = 0; i < outframes; i++) {
        int srcindex = (int)(Sint32)(srcpos >> 32);
        Uint32 srcfraction = (Uint32)(srcpos & 0xFFFFFFFF);
        srcpos += resample_rate;

        SDL_assert(srcindex >= -1 && srcindex < inframes);

        const float *filter = &ResamplerFilter[(srcfraction >> RESAMPLER_FILTER_INTERP_BITS) * RESAMPLER_SAMPLES_PER_FRAME];
        const float interp = (float)(srcfraction & (RESAMPLER_FILTER_INTERP_RANGE - 1)) * (1.0f / RESAMPLER_FILTER_INTERP_RANGE);

        const float *frame = &src[srcindex * chans];
        resample_frame(frame, dst, filter, interp, chans);

        dst += chans;
    }

    *inout_resample_offset = srcpos - ((Sint64)inframes << 32);
}
