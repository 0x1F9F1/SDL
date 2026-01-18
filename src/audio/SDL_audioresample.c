/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

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

// If you really need higher quality than this, SDL probably isn't want you want.
#define RESAMPLER_MIN_SAMPLES_PER_WING  4
#define RESAMPLER_MAX_SAMPLES_PER_WING  128
#define RESAMPLER_MAX_SAMPLES_PER_FRAME (RESAMPLER_MAX_SAMPLES_PER_WING * 2)

// The minimum number of samples_per_wing before attempting to apply a lowpass filter when downsampling
#define RESAMPLER_MIN_SAMPLES_FOR_LOWPASS 8

// ResampleFrame is just a vector/matrix/matrix multiplication.
// It performs cubic interpolation of the filter, then multiplies that with the input.
// dst = [1, frac, frac^2, frac^3] * filter * src

// Cubic Polynomial
typedef union Cubic
{
    float v[4];

#ifdef SDL_SSE_INTRINSICS
    // Aligned loads can be used directly as memory operands for mul/add
    __m128 v128;
#endif

#ifdef SDL_NEON_INTRINSICS
    float32x4_t v128;
#endif

} Cubic;

#define RESAMPLE_FRAMES_ARGS const SDL_AudioResampler *resampler, int chans, const float *src, int inframes, float *dst, int outframes, \
                             Sint64 resample_rate, Sint64 *inout_resample_offset

typedef void (*ResampleFramesFunction)(RESAMPLE_FRAMES_ARGS);

struct SDL_AudioResampler
{
    int samples_per_wing;
    int samples_per_frame; // = samples_per_wing * 2
    void (*destroy)(SDL_AudioResampler *resampler);
    ResampleFramesFunction resample_frames;
};

typedef struct SDL_WindowedAudioResampler
{
    SDL_AudioResampler parent;
    int frac_bits;
    Uint32 frac_mask;
    float frac_scale;
    Cubic *filter;
} SDL_WindowedAudioResampler;

// Calculate the cubic equation which passes through all four points.
// https://en.wikipedia.org/wiki/Ordinary_least_squares
// https://en.wikipedia.org/wiki/Polynomial_regression
static void CubicLeastSquares(Cubic *coeffs, float y0, float y1, float y2, float y3)
{
    // Least squares matrix for xs = [0, 1/3, 2/3, 1]
    // [  1.0   0.0   0.0  0.0 ]
    // [ -5.5   9.0  -4.5  1.0 ]
    // [  9.0 -22.5  18.0 -4.5 ]
    // [ -4.5  13.5 -13.5  4.5 ]

    coeffs->v[0] = y0;
    coeffs->v[1] = -5.5f * y0 + 9.0f * y1 - 4.5f * y2 + y3;
    coeffs->v[2] = 9.0f * y0 - 22.5f * y1 + 18.0f * y2 - 4.5f * y3;
    coeffs->v[3] = -4.5f * y0 + 13.5f * y1 - 13.5f * y2 + 4.5f * y3;
}

// Transpose 4x4 floats
static void Transpose4x4(Cubic *data)
{
    Cubic temp[4] = { data[0], data[1], data[2], data[3] };

    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            data[i].v[j] = temp[j].v[i];
        }
    }
}

/*
# SageMath code https://sagecell.sagemath.org

# Generate a polynomial approximation
def oversample(degree, samples, bits, func):
    m = matrix(vector((i/samples)^j for j in xsrange(degree+1)) for i in xsrange(samples+1))
    m = (m.T * m).solve_right(m.T)
    rate = 2^bits
    size = rate * samples
    values = [ func(i, size) for i in srange(size+1) ]
    for i in range(0, size, samples):
        src = vector(values[i:i+samples+1])
        dst = m * src
        print('{ ' + ', '.join(f'{float(v):>+16.9e}f' for v in dst) + ' },')

def kaiser_beta(dB):
    if dB > 50.0:
        return 0.1102 * (dB - 8.7)
    elif (dB >= 21.0):
        return 0.5842 * ((dB - 21.0) ^ 0.4) + (0.07886 * (dB - 21.0))
    else:
        return 0.0

# Generate one wing of the kaiser window https://en.wikipedia.org/wiki/Kaiser_window
def kaiser_window(dB):
    beta = kaiser_beta(dB)
    bessel_beta = bessel_I(0, beta)
    oversample(5, 5, 3, lambda i, size: bessel_I(0, beta * sqrt(1 - (i/size)^2)) / bessel_beta)
*/

static float EvalPolynomial(const float *values, int degree, float frac)
{
    float result = values[degree];

    for (int i = degree; i > 0; --i) {
        result = (result * frac) + values[i - 1];
    }

    return result;
}

// oversample(5, 5, 2, lambda x, size: sin((pi * x) / (size * 2)) / pi)
#define SINC_DEGREE 5
#define SINC_BITS   2
#define SINC_RATE   (1 << SINC_BITS)

static const float SincQuadrant[SINC_RATE][SINC_DEGREE + 1] = {
    { +0.000000000e+00f, +1.249999896e-01f, +1.154605931e-07f, -3.213210660e-03f, +7.602026351e-07f, +2.426521292e-05f },
    { +1.218119198e-01f, +1.154849085e-01f, -9.392089831e-03f, -2.969721405e-03f, +1.234909386e-04f, +2.057105423e-05f },
    { +2.250790790e-01f, +8.838829694e-02f, -1.735443459e-02f, -2.274118986e-03f, +2.274212986e-04f, +1.374513900e-05f },
    { +2.940799888e-01f, +4.783536842e-02f, -2.267472399e-02f, -1.232302567e-03f, +2.967288274e-04f, +4.826650937e-06f },
};

// Calculate `sin(pi*x)/(pi*x)` for `x > 0`
static float Sinc(float x)
{
    const int SINC_FRAC = SINC_RATE - 1;
    const int SINC_HALF = SINC_RATE;
    const int SINC_ONE = SINC_RATE * 2;

    float scaled = x * (float)SINC_ONE;
    int whole = (int)scaled;
    float frac = scaled - (float)whole;

    // Reverse 2nd and 4th quadrants
    if (whole & SINC_HALF) {
        whole ^= SINC_FRAC;
        frac = 1.0f - frac;
    }

    float result = EvalPolynomial(SincQuadrant[whole & SINC_FRAC], SINC_DEGREE, frac);

    // Negate 3rd and 4th quadrants
    result = (whole & SINC_ONE) ? -result : result;

    return result / x;
}

#define KAISER_DEGREE 5
#define KAISER_BITS   3
#define KAISER_RATE   (1 << KAISER_BITS)

// kaiser_window(80)
static const float Kaiser80[KAISER_RATE][KAISER_DEGREE + 1] = {
    { +1.000000000e+00f, -7.648698244e-07f, -5.732525584e-02f, -3.610928377e-05f, +1.504846224e-03f, -6.144399785e-05f },
    { +9.440812722e-01f, -1.090485776e-01f, -4.902261579e-02f, +5.315126956e-03f, +1.173409258e-03f, -1.527421110e-04f },
    { +7.923458730e-01f, -1.872186924e-01f, -2.756941026e-02f, +8.472400424e-03f, +3.753204124e-04f, -1.676817151e-04f },
    { +5.862378094e-01f, -2.162766646e-01f, -1.580942310e-03f, +8.333474901e-03f, -4.884096536e-04f, -1.050408344e-04f },
    { +3.761202269e-01f, -1.969157259e-01f, +1.943805502e-02f, +5.388064841e-03f, -1.017299004e-03f, -5.105031043e-06f },
    { +2.030082168e-01f, -1.459691174e-01f, +2.944996919e-02f, +1.316167892e-03f, -1.025887561e-03f, +7.694474364e-05f },
    { +8.685629366e-02f, -8.683915620e-02f, +2.801638322e-02f, -2.001729234e-03f, -6.161595839e-04f, +1.039562138e-04f },
    { +2.551958808e-02f, -3.875678341e-02f, +1.935652461e-02f, -3.443395293e-03f, -7.841097883e-05f, +7.509780541e-05f },
};

static float KaiserWindow(const float *window, float x)
{
    float scaled = x * (float)KAISER_RATE;
    int whole = (int)scaled;
    float frac = scaled - (float)whole;

    if (whole >= KAISER_RATE)
        return 0.0f;

    return EvalPolynomial(&window[whole * (KAISER_DEGREE + 1)], KAISER_DEGREE, frac);
}

static Cubic *CreateCubicFilter(int samples_per_wing, int oversample_bits, const float *window, float lpf, bool transpose)
{
    const int samples_per_frame = samples_per_wing * 2;

    // Oversample at 3x the target rate, so that we have samples at [0, 1/3, 2/3, 1] of each position
    const int oversample_rate = 1 << oversample_bits;
    const int table_oversample_rate = oversample_rate * 3;
    const int table_size = samples_per_wing * table_oversample_rate;
    const int filter_size = samples_per_frame * oversample_rate;

    // Generate one wing of the filter
    // https://en.wikipedia.org/wiki/Whittaker%E2%80%93Shannon_interpolation_formula

    float *table = SDL_malloc((table_size + 1) * sizeof(*table));
    table[0] = lpf;
    table[table_size] = 0.0f;

    for (int i = 1; i < table_size; ++i) {
        float x = (float)i;
        table[i] = lpf * KaiserWindow(window, x / table_size) * Sinc((x * lpf) / table_oversample_rate);
    }

    Cubic *filter = SDL_aligned_alloc(SDL_GetSIMDAlignment(), sizeof(*filter) * filter_size);

#define GET_CUBIC(INDEX, SAMPLE) &filter[(INDEX) * samples_per_frame + (SAMPLE)]

    // Generate the coefficients for each point
    // When interpolating, the fraction represents how far we are between input samples,
    // so we need to align the filter by "moving" it to the right.
    //
    // For the left wing, this means interpolating "forwards" (away from the center)
    // For the right wing, this means interpolating "backwards" (towards the center)
    //
    // The center of the filter is at the end of the left wing (RESAMPLER_SAMPLES_PER_WING - 1)
    // The left wing is the filter, but reversed
    // The right wing is the filter, but offset by 1
    //
    // Since the right wing is offset by 1, this just means we interpolate backwards
    // between the same points, instead of forwards
    // interp(p[n], p[n+1], t) = interp(p[n+1], p[n+1-1], 1 - t) = interp(p[n+1], p[n], 1 - t)
    for (int i = 0; i < oversample_rate; ++i) {
        for (int j = 0; j < samples_per_wing; ++j) {
            const float *ys = &table[((j * oversample_rate) + i) * 3];

            Cubic *fwd = GET_CUBIC(i, samples_per_wing - j - 1);
            Cubic *rev = GET_CUBIC(oversample_rate - i - 1, samples_per_wing + j);

            // Calculate the cubic equation of the 4 points
            CubicLeastSquares(fwd, ys[0], ys[1], ys[2], ys[3]);
            CubicLeastSquares(rev, ys[3], ys[2], ys[1], ys[0]);
        }
    }

    SDL_free(table);

    if (transpose) {
        // Transpose each set of 4 coefficients, to reduce work when resampling
        for (int i = 0; i < oversample_rate; ++i) {
            for (int j = 0; j + 4 <= samples_per_frame; j += 4) {
                Transpose4x4(GET_CUBIC(i, j));
            }
        }
    }

#undef GET_CUBIC

    return filter;
}

#define BEGIN_RESAMPLE_FUNCTION                                                 \
    src -= (resampler->samples_per_wing - 1) * chans;                           \
    Sint64 srcpos = *inout_resample_offset;                                     \
    SDL_WindowedAudioResampler *self = (SDL_WindowedAudioResampler *)resampler; \
    const int samples_per_frame = resampler->samples_per_frame;                 \
    Cubic *active_filter = self->filter

#define BEGIN_RESAMPLE_LOOP                                                                         \
    for (int outframe = 0; outframe < outframes; ++outframe, dst += chans) {                        \
        int srcindex = (int)(Sint32)(srcpos >> 32);                                                 \
        Uint32 srcfraction = (Uint32)(srcpos & 0xFFFFFFFF);                                         \
        srcpos += resample_rate;                                                                    \
        SDL_assert(srcindex >= -1 && srcindex < inframes);                                          \
        const Cubic *filter = &active_filter[(srcfraction >> self->frac_bits) * samples_per_frame]; \
        const float frac = (float)(srcfraction & self->frac_mask) * self->frac_scale;               \
        const float *frame = &src[srcindex * chans];

#define END_RESAMPLE_LOOP \
    }                     \
    *inout_resample_offset = srcpos - ((Sint64)inframes << 32);

#define INTERP_CUBIC(VALUE) (((VALUE)->v[0] + ((VALUE)->v[1] * frac)) + (((VALUE)->v[2] * frac2) + ((VALUE)->v[3] * frac3)))

#define BEGIN_RESAMPLE_LOOP_SCALAR   \
    BEGIN_RESAMPLE_LOOP              \
    const float frac2 = frac * frac; \
    const float frac3 = frac * frac2;

static void WindowedResampleFrames_Scalar(RESAMPLE_FRAMES_ARGS)
{
    BEGIN_RESAMPLE_FUNCTION;

    if (chans == 2) {
        BEGIN_RESAMPLE_LOOP_SCALAR
        {
            float out0 = 0.0f;
            float out1 = 0.0f;

            for (int i = 0; i < samples_per_frame; ++i) {
                const float scale = INTERP_CUBIC(&filter[i]);
                out0 += frame[0] * scale;
                out1 += frame[1] * scale;
                frame += 2;
            }

            dst[0] = out0;
            dst[1] = out1;
        }
        END_RESAMPLE_LOOP
    } else if (chans == 1) {
        BEGIN_RESAMPLE_LOOP_SCALAR
        {
            float out = 0.0f;

            for (int i = 0; i < samples_per_frame; ++i) {
                out += frame[i] * INTERP_CUBIC(&filter[i]);
            }

            dst[0] = out;
        }
        END_RESAMPLE_LOOP
    } else {
        float scales[RESAMPLER_MAX_SAMPLES_PER_FRAME];

        BEGIN_RESAMPLE_LOOP_SCALAR
        {
            for (int i = 0; i < samples_per_frame; ++i) {
                scales[i] = INTERP_CUBIC(&filter[i]);
            }

            for (int chan = 0; chan < chans; ++chan) {
                float out = 0.0f;

                for (int i = 0; i < samples_per_frame; ++i) {
                    out += frame[i * chans + chan] * scales[i];
                }

                dst[chan] = out;
            }
        }
        END_RESAMPLE_LOOP
    }
}

#undef INTERP_CUBIC
#undef BEGIN_RESAMPLE_LOOP_SCALAR

#ifdef SDL_SSE_INTRINSICS

#define sdl_madd_ps(a, b, c) _mm_add_ps(a, _mm_mul_ps(b, c)) // Not-so-fused multiply-add

#define BEGIN_RESAMPLE_LOOP_SSE                    \
    BEGIN_RESAMPLE_LOOP                            \
    const __m128 frac1 = _mm_set1_ps(frac);        \
    const __m128 frac2 = _mm_mul_ps(frac1, frac1); \
    const __m128 frac3 = _mm_mul_ps(frac1, frac2);

// Transposed in SetupAudioResampler
// Explicitly use _mm_load_ps to workaround ICE in GCC 4.9.4 accessing Cubic.v128
#define INTERP_CUBIC_SSE(VALUE) \
    sdl_madd_ps(sdl_madd_ps(sdl_madd_ps(_mm_load_ps((VALUE)[0].v), frac1, _mm_load_ps((VALUE)[1].v)), frac2, _mm_load_ps((VALUE)[2].v)), frac3, _mm_load_ps((VALUE)[3].v))

static void SDL_TARGETING("sse") WindowedResampleFrames_SSE(RESAMPLE_FRAMES_ARGS)
{
    BEGIN_RESAMPLE_FUNCTION;
    const int vectors_per_frame = samples_per_frame / 4;

    if (chans == 2) {
        BEGIN_RESAMPLE_LOOP_SSE
        {
            // Use two accumulators to improve throughput
            __m128 out0 = _mm_setzero_ps();
            __m128 out1 = _mm_setzero_ps();

            for (int i = 0; i < vectors_per_frame; ++i) {
                __m128 scale = INTERP_CUBIC_SSE(&filter[i * 4]);
                out0 = sdl_madd_ps(out0, _mm_loadu_ps(&frame[i * 8 + 0]), _mm_unpacklo_ps(scale, scale));
                out1 = sdl_madd_ps(out1, _mm_loadu_ps(&frame[i * 8 + 4]), _mm_unpackhi_ps(scale, scale));
            }

            // Add the accumulators together
            __m128 out = _mm_add_ps(out0, out1);

            // Add the lower and upper pairs together
            out = _mm_add_ps(out, _mm_movehl_ps(out, out));

            // Store the result
            _mm_storel_pi((__m64 *)dst, out);
        }
        END_RESAMPLE_LOOP
    } else if (chans == 1) {
        BEGIN_RESAMPLE_LOOP_SSE
        {
            __m128 out = _mm_setzero_ps();

            for (int i = 0; i < vectors_per_frame; ++i) {
                out = sdl_madd_ps(out, _mm_loadu_ps(&frame[i * 4]), INTERP_CUBIC_SSE(&filter[i * 4]));
            }

            // Horizontal sum
            __m128 shuf = _mm_shuffle_ps(out, out, _MM_SHUFFLE(2, 3, 0, 1));
            out = _mm_add_ps(out, shuf);
            out = _mm_add_ss(out, _mm_movehl_ps(shuf, out));

            _mm_store_ss(dst, out);
        }
        END_RESAMPLE_LOOP
    } else {
        __m128 scales[RESAMPLER_MAX_SAMPLES_PER_FRAME / 4];

        BEGIN_RESAMPLE_LOOP_SSE
        {
            for (int i = 0; i < vectors_per_frame; ++i) {
                scales[i] = INTERP_CUBIC_SSE(&filter[i * 4]);
            }

            int chan = 0;

            // Process 4 channels at once
            for (; chan + 4 <= chans; chan += 4) {
                const float *in = &frame[chan];
                __m128 out0 = _mm_setzero_ps();
                __m128 out1 = _mm_setzero_ps();

#define X(a, b, out)                                                                         \
    out = sdl_madd_ps(out, _mm_loadu_ps(in), _mm_shuffle_ps(a, a, _MM_SHUFFLE(b, b, b, b))); \
    in += chans

                for (int i = 0; i < vectors_per_frame; ++i) {
                    __m128 scale = scales[i];
                    X(scale, 0, out0);
                    X(scale, 1, out1);
                    X(scale, 2, out0);
                    X(scale, 3, out1);
                }

#undef X

                // Add the accumulators together
                __m128 out = _mm_add_ps(out0, out1);
                _mm_storeu_ps(&dst[chan], out);
            }

            // Process the remaining channels one at a time.
            // Channel counts 1,2,4,8 are already handled above, leaving 3,5,6,7 to deal with (looping 3,1,2,3 times).
            // Without vgatherdps (AVX2), this gets quite messy.
            for (; chan < chans; ++chan) {
                const float *in = &frame[chan];
                __m128 out = _mm_setzero_ps();

                for (int i = 0; i < vectors_per_frame; ++i) {
                    __m128 x = _mm_unpacklo_ps(_mm_load_ss(in), _mm_load_ss(in + chans));
                    in += chans + chans;
                    x = _mm_movelh_ps(x, _mm_unpacklo_ps(_mm_load_ss(in), _mm_load_ss(in + chans)));
                    in += chans + chans;
                    out = sdl_madd_ps(out, x, scales[i]);
                }

                // Horizontal sum
                __m128 shuf = _mm_shuffle_ps(out, out, _MM_SHUFFLE(2, 3, 0, 1));
                out = _mm_add_ps(out, shuf);
                out = _mm_add_ss(out, _mm_movehl_ps(shuf, out));

                _mm_store_ss(&dst[chan], out);
            }
        }
        END_RESAMPLE_LOOP
    }
}

#undef INTERP_CUBIC_SSE
#undef BEGIN_RESAMPLE_LOOP_SSE
#undef sdl_madd_ps

#endif

#ifdef SDL_NEON_INTRINSICS

#define BEGIN_RESAMPLE_LOOP_NEON                       \
    BEGIN_RESAMPLE_LOOP                                \
    const float32x4_t frac1 = vdupq_n_f32(frac);       \
    const float32x4_t frac2 = vmulq_f32(frac1, frac1); \
    const float32x4_t frac3 = vmulq_f32(frac1, frac2);

#define INTERP_CUBIC_NEON(VALUE) \
    vmlaq_f32(vmlaq_f32(vmlaq_f32((VALUE)[0].v128, frac1, (VALUE)[1].v128), frac2, (VALUE)[2].v128), frac3, (VALUE)[3].v128)

static void WindowedResampleFrames_NEON(RESAMPLE_FRAMES_ARGS)
{
    BEGIN_RESAMPLE_FUNCTION;
    const int vectors_per_frame = samples_per_frame / 4;

    if (chans == 2) {
        BEGIN_RESAMPLE_LOOP_NEON
        {
            // Use two accumulators to improve throughput
            float32x4_t out0 = vdupq_n_f32(0);
            float32x4_t out1 = vdupq_n_f32(0);

            for (int i = 0; i < vectors_per_frame; ++i) {
                float32x4_t scale = INTERP_CUBIC_NEON(&filter[i * 4]);
                float32x4x2_t scale2 = vzipq_f32(scale, scale);
                out0 = vmlaq_f32(out0, vld1q_f32(&frame[i * 8 + 0]), scale2.val[0]);
                out1 = vmlaq_f32(out1, vld1q_f32(&frame[i * 8 + 4]), scale2.val[1]);
            }

            // Add the accumulators together
            out0 = vaddq_f32(out0, out1);

            // Add the lower and upper pairs together
            float32x2_t out = vadd_f32(vget_low_f32(out0), vget_high_f32(out0));

            // Store the result
            vst1_f32(dst, out);
        }
        END_RESAMPLE_LOOP
    } else if (chans == 1) {
        BEGIN_RESAMPLE_LOOP_NEON
        {
            float32x4_t out = vdupq_n_f32(0);

            for (int i = 0; i < vectors_per_frame; ++i) {
                out = vmlaq_f32(out, vld1q_f32(&frame[i * 4]), INTERP_CUBIC_NEON(&filter[i * 4]));
            }

            // Horizontal sum
            float32x2_t sum = vadd_f32(vget_low_f32(out), vget_high_f32(out));
            sum = vpadd_f32(sum, sum);

            vst1_lane_f32(dst, sum, 0);
        }
        END_RESAMPLE_LOOP
    } else {
        float32x4_t scales[RESAMPLER_MAX_SAMPLES_PER_FRAME / 4];

        BEGIN_RESAMPLE_LOOP_NEON
        {
            for (int i = 0; i < vectors_per_frame; ++i) {
                scales[i] = INTERP_CUBIC_NEON(&filter[i * 4]);
            }

            int chan = 0;

            // Process 4 channels at once
            for (; chan + 4 <= chans; chan += 4) {
                const float *in = &frame[chan];
                float32x4_t out0 = vdupq_n_f32(0);
                float32x4_t out1 = vdupq_n_f32(0);

#define X(a, b, out)                                           \
    out = vmlaq_f32(out, vld1q_f32(in), vdupq_lane_f32(a, b)); \
    in += chans

                for (int i = 0; i < vectors_per_frame; ++i) {
                    float32x4_t scale = scales[i];
                    X(vget_low_f32(scale), 0, out0);
                    X(vget_low_f32(scale), 1, out1);
                    X(vget_high_f32(scale), 0, out0);
                    X(vget_high_f32(scale), 1, out1);
                }

#undef X

                // Add the accumulators together
                float32x4_t out = vaddq_f32(out0, out1);

                vst1q_f32(&dst[chan], out);
            }

            // Process the remaining channels one at a time.
            // Channel counts 1,2,4,8 are already handled above, leaving 3,5,6,7 to deal with (looping 3,1,2,3 times).
            for (; chan < chans; ++chan) {
                const float *in = &frame[chan];
                float32x4_t out = vdupq_n_f32(0);

                for (int i = 0; i < vectors_per_frame; ++i) {
                    float32x4_t x = vld1q_dup_f32(in);
                    in += chans;
                    x = vld1q_lane_f32(in, x, 1);
                    in += chans;
                    x = vld1q_lane_f32(in, x, 2);
                    in += chans;
                    x = vld1q_lane_f32(in, x, 3);
                    in += chans;

                    out = vmlaq_f32(out, x, scales[i]);
                }

                // Horizontal sum
                float32x2_t sum = vadd_f32(vget_low_f32(out), vget_high_f32(out));
                sum = vpadd_f32(sum, sum);

                vst1_lane_f32(&dst[chan], sum, 0);
            }
        }
        END_RESAMPLE_LOOP
    }
}

#undef INTERP_CUBIC_NEON
#undef BEGIN_RESAMPLE_LOOP_NEON

#endif

static void DestroyWindowedAudioResampler(SDL_AudioResampler *resampler)
{
    SDL_WindowedAudioResampler *self = (SDL_WindowedAudioResampler *)resampler;
    SDL_aligned_free(self->filter);
    SDL_free(self);
}

static SDL_AudioResampler *CreateWindowedAudioResampler(ResampleFramesFunction resample_frames, int samples_per_wing, const float *window, float lpf, bool transpose)
{
    // Round up to the next multiple of 2 (so that samples_per_frame is a multiple of 4 for SIMD)
    if (samples_per_wing % 2)
        ++samples_per_wing;

    // And clamp to the valid range.
    samples_per_wing = SDL_clamp(samples_per_wing, RESAMPLER_MIN_SAMPLES_PER_WING, RESAMPLER_MAX_SAMPLES_PER_WING);

    SDL_WindowedAudioResampler *filter = SDL_malloc(sizeof(*filter));
    filter->parent.samples_per_wing = samples_per_wing;
    filter->parent.samples_per_frame = samples_per_wing * 2;
    filter->parent.destroy = DestroyWindowedAudioResampler;
    filter->parent.resample_frames = resample_frames;

    // More bits gives more precision, at the cost of a larger table.
    const int oversample_bits = 3;
    const int frac_bits = 32 - oversample_bits;
    const Uint32 frac_range = (Uint32)1 << frac_bits;
    filter->frac_bits = frac_bits;
    filter->frac_mask = frac_range - 1;
    filter->frac_scale = 1.0f / frac_range;

    filter->filter = CreateCubicFilter(samples_per_wing, oversample_bits, window, lpf, transpose);

    return &filter->parent;
}

static void DestroyAudioResampler(SDL_AudioResampler *resampler)
{
    if (resampler) {
        resampler->destroy(resampler);
    }
}

static SDL_InitState SDL_resampler_init;
static SDL_AudioResampler *SDL_default_resampler;

void SDL_SetupAudioResampler(void)
{
    if (!SDL_ShouldInit(&SDL_resampler_init)) {
        return;
    }

    ResampleFramesFunction resample_frames = WindowedResampleFrames_Scalar;
    int samples_per_wing = 8;
    const float *window = Kaiser80[0];
    float lpf = 1.0f;
    bool transpose = false;

#ifdef SDL_SSE_INTRINSICS
    if (SDL_HasSSE()) {
        resample_frames = WindowedResampleFrames_SSE;
        samples_per_wing = 16;
        transpose = true;
    }
#endif

#ifdef SDL_NEON_INTRINSICS
    if (SDL_HasNEON()) {
        resample_frames = WindowedResampleFrames_NEON;
        samples_per_wing = 16;
        transpose = true;
    }
#endif

    SDL_default_resampler = CreateWindowedAudioResampler(resample_frames, samples_per_wing, window, lpf, transpose);

    SDL_SetInitialized(&SDL_resampler_init, true);
}

void SDL_QuitAudioResampler()
{
    if (!SDL_ShouldQuit(&SDL_resampler_init)) {
        return;
    }

    DestroyAudioResampler(SDL_default_resampler);
    SDL_default_resampler = NULL;

    SDL_SetInitialized(&SDL_resampler_init, false);
}

SDL_AudioResampler *SDL_GetDefaultAudioResampler()
{
    SDL_SetupAudioResampler();

    return SDL_default_resampler;
}

Sint64 SDL_GetResampleRate(int src_rate, int dst_rate)
{
    SDL_assert(src_rate > 0);
    SDL_assert(dst_rate > 0);

    Sint64 numerator = (Sint64)src_rate << 32;
    Sint64 denominator = (Sint64)dst_rate;

    // Generally it's expected that `dst_frames = (src_frames * dst_rate) / src_rate`
    // To match this as closely as possible without infinite precision, always round up the resample rate.
    // For example, without rounding up, a sample ratio of 2:3 would have `sample_rate = 0xAAAAAAAA`
    // After 3 frames, the position would be 0x1.FFFFFFFE, meaning we haven't fully consumed the second input frame.
    // By rounding up to 0xAAAAAAAB, we would instead reach 0x2.00000001, fulling consuming the second frame.
    // Technically you could say this is kicking the can 0x100000000 steps down the road, but I'm fine with that :)
    // sample_rate = div_ceil(numerator, denominator)
    Sint64 sample_rate = ((numerator - 1) / denominator) + 1;

    SDL_assert(sample_rate > 0);

    return sample_rate;
}

// `src[srcindex + sample]` is sampled, where `-1 <= srcindex <= (inframes - 1)` and `-(samples_per_wing - 1) <= sample <= samples_per_wing`
//
// Minimum Index:
// srcindex = -1, sample = -(samples_per_wing - 1)
// src[-1 + -(samples_per_wing - 1)] = src[-samples_per_wing]
//
// Maxmimum Index:
// srcindex = (inframes - 1), sample = samples_per_wing
// src[inframes + samples_per_wing - 1] = (src + inframes)[samples_per_wing - 1]
int SDL_GetResamplerPaddingFrames(SDL_AudioResampler *resampler)
{
    return resampler->samples_per_wing;
}

// These are not general purpose. They do not check for all possible underflow/overflow
SDL_FORCE_INLINE bool ResamplerAdd(Sint64 a, Sint64 b, Sint64 *ret)
{
    if ((b > 0) && (a > SDL_MAX_SINT64 - b)) {
        return false;
    }

    *ret = a + b;
    return true;
}

SDL_FORCE_INLINE bool ResamplerMul(Sint64 a, Sint64 b, Sint64 *ret)
{
    if ((b > 0) && (a > SDL_MAX_SINT64 / b)) {
        return false;
    }

    *ret = a * b;
    return true;
}

Sint64 SDL_GetResamplerInputFrames(Sint64 output_frames, Sint64 resample_rate, Sint64 resample_offset)
{
    // Calculate the index of the last input frame, then add 1.
    // ((((output_frames - 1) * resample_rate) + resample_offset) >> 32) + 1

    Sint64 output_offset;
    if (!ResamplerMul(output_frames, resample_rate, &output_offset) ||
        !ResamplerAdd(output_offset, -resample_rate + resample_offset + 0x100000000, &output_offset)) {
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
    if (!ResamplerMul(input_frames, 0x100000000, &input_offset) ||
        !ResamplerAdd(input_offset, -resample_offset, &input_offset)) {
        input_offset = SDL_MAX_SINT64;
    }

    // output_frames = div_ceil(input_offset, resample_rate)
    Sint64 output_frames = (input_offset > 0) ? ((input_offset - 1) / resample_rate) + 1 : 0;

    *inout_resample_offset = (output_frames * resample_rate) - input_offset;

    return output_frames;
}

void SDL_ResampleAudio(SDL_AudioResampler *resampler, int chans, const float *src, int inframes, float *dst, int outframes,
                       Sint64 resample_rate, Sint64 *inout_resample_offset)
{
    SDL_assert(resample_rate > 0);

    resampler->resample_frames(resampler, chans, src, inframes, dst, outframes, resample_rate, inout_resample_offset);
}
