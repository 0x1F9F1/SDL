/*
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_test.h>

#include "testutils.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN
#include <emscripten/emscripten.h>
#endif

#include <stdlib.h>

#define SLIDER_WIDTH_PERC  500.f / 600.f
#define SLIDER_HEIGHT_PERC 25.f / 480.f

static int done;
static SDLTest_CommonState *state;

static SDL_AudioSpec spec;
static SDL_AudioStream *src_stream;
static SDL_AudioStream *tmp_stream;
static SDL_AudioStream *dst_stream;
static SDL_AudioResampler *src_resampler;
static SDL_AudioResampler *tmp_resampler;

static Uint8 *audio_buf = NULL;
static Uint32 audio_len = 0;

static bool auto_loop = true;
static bool auto_flush = false;

static Uint64 last_get_callback = 0;
static Uint64 last_get_timing = 0;
static int last_get_amount_additional = 0;
static int last_get_amount_total = 0;

typedef struct Slider
{
    SDL_FRect area;
    bool changed;
    char fmtlabel[64];
    float pos;
    int flags;
    float min;
    float mid;
    float max;
    float value;
} Slider;

#define NUM_SLIDERS 9
Slider sliders[NUM_SLIDERS];
static int active_slider = -1;

static void init_slider(int index, const char *fmtlabel, int flags, float value, float min, float max)
{
    Slider *slider = &sliders[index];

    slider->area.x = state->window_w * (1.f - SLIDER_WIDTH_PERC) / 2;
    slider->area.y = state->window_h * (0.2f + (index * SLIDER_HEIGHT_PERC * 1.4f));
    slider->area.w = SLIDER_WIDTH_PERC * state->window_w;
    slider->area.h = SLIDER_HEIGHT_PERC * state->window_h;
    slider->changed = true;
    SDL_strlcpy(slider->fmtlabel, fmtlabel, SDL_arraysize(slider->fmtlabel));
    slider->flags = flags;
    slider->min = min;
    slider->max = max;
    slider->value = value;

    if (slider->flags & 4) {
        slider->pos = (value - slider->min) / (slider->max - slider->min);
    } else if (slider->flags & 1) {
        slider->pos = (value - slider->min + 0.5f) / (slider->max - slider->min + 1.0f);
    } else {
        slider->pos = 0.5f;
        slider->mid = value;
    }
}

static float lerp(float v0, float v1, float t)
{
    return (1 - t) * v0 + t * v1;
}

static void draw_text(SDL_Renderer *renderer, int x, int y, const char *text)
{
    SDL_SetRenderDrawColor(renderer, 0xFD, 0xF6, 0xE3, 0xFF);
    SDLTest_DrawString(renderer, (float)x, (float)y, text);
}

static void draw_textf(SDL_Renderer *renderer, int x, int y, const char *fmt, ...)
{
    char text[256];
    va_list ap;

    va_start(ap, fmt);
    SDL_vsnprintf(text, SDL_arraysize(text), fmt, ap);
    va_end(ap);

    draw_text(renderer, x, y, text);
}

static void skip_audio(float amount)
{
    float speed;
    SDL_AudioSpec dst_spec;
    int num_bytes;
    int result = 0;
    void *buf = NULL;

    SDL_LockAudioStream(src_stream);

    speed = SDL_GetAudioStreamFrequencyRatio(src_stream);
    SDL_GetAudioStreamFormat(src_stream, NULL, &dst_spec);

    SDL_SetAudioStreamFrequencyRatio(src_stream, 100.0f);

    num_bytes = (int)(SDL_AUDIO_FRAMESIZE(dst_spec) * dst_spec.freq * ((speed * amount) / 100.0f));
    buf = SDL_malloc(num_bytes);

    if (buf) {
        result = SDL_GetAudioStreamData(src_stream, buf, num_bytes);
        SDL_free(buf);
    }

    SDL_SetAudioStreamFrequencyRatio(src_stream, speed);

    SDL_UnlockAudioStream(src_stream);

    if (result >= 0) {
        SDL_Log("Skipped %.2f seconds", amount);
    } else {
        SDL_Log("Failed to skip: %s", SDL_GetError());
    }
}

static const char *AudioChansToStr(const int channels)
{
    switch (channels) {
    case 1:
        return "Mono";
    case 2:
        return "Stereo";
    case 3:
        return "2.1";
    case 4:
        return "Quad";
    case 5:
        return "4.1";
    case 6:
        return "5.1";
    case 7:
        return "6.1";
    case 8:
        return "7.1";
    default:
        break;
    }
    return "?";
}

static void scale_mouse_coords(SDL_FPoint *p)
{
    SDL_Window *window = SDL_GetMouseFocus();
    if (window) {
        int w, p_w;
        float scale;
        SDL_GetWindowSize(window, &w, NULL);
        SDL_GetWindowSizeInPixels(window, &p_w, NULL);
        scale = (float)p_w / (float)w;
        p->x *= scale;
        p->y *= scale;
    }
}

static void loop(void)
{
    int i, j;
    SDL_Event e;
    SDL_FPoint p;
    SDL_AudioSpec src_spec, tmp_spec, dst_spec;
    int queued_bytes = 0;
    int available_bytes = 0;
    float available_seconds = 0;

    while (SDL_PollEvent(&e)) {
        SDLTest_CommonEvent(state, &e, &done);
#ifdef SDL_PLATFORM_EMSCRIPTEN
        if (done) {
            emscripten_cancel_main_loop();
        }
#endif
        if (e.type == SDL_EVENT_KEY_DOWN) {
            SDL_Keycode key = e.key.key;
            if (key == SDLK_Q) {
                if (SDL_AudioDevicePaused(state->audio_id)) {
                    SDL_ResumeAudioDevice(state->audio_id);
                } else {
                    SDL_PauseAudioDevice(state->audio_id);
                }
            } else if (key == SDLK_W) {
                auto_loop = !auto_loop;
            } else if (key == SDLK_E) {
                auto_flush = !auto_flush;
            } else if (key == SDLK_A) {
                SDL_ClearAudioStream(src_stream);
                SDL_Log("Cleared audio stream");
            } else if (key == SDLK_D) {
                float amount = 1.0f;
                amount *= (e.key.mod & SDL_KMOD_CTRL) ? 10.0f : 1.0f;
                amount *= (e.key.mod & SDL_KMOD_SHIFT) ? 10.0f : 1.0f;
                skip_audio(amount);
            }
        }
    }

    if (SDL_GetMouseState(&p.x, &p.y) & SDL_BUTTON_LMASK) {
        scale_mouse_coords(&p);
        if (active_slider == -1) {
            for (i = 0; i < NUM_SLIDERS; ++i) {
                if (SDL_PointInRectFloat(&p, &sliders[i].area)) {
                    active_slider = i;
                    break;
                }
            }
        }
    } else {
        active_slider = -1;
    }

    if (active_slider != -1) {
        Slider *slider = &sliders[active_slider];

        float value = (p.x - slider->area.x) / slider->area.w;
        value = SDL_clamp(value, 0.0f, 1.0f);
        slider->pos = value;

        if (slider->flags & 4) {
            value = slider->min + (value * (slider->max - slider->min));
            value = SDL_clamp(value, slider->min, slider->max);
        } else if (slider->flags & 1) {
            value = slider->min + (value * (slider->max - slider->min + 1.0f));
            value = SDL_clamp(value, slider->min, slider->max);
        } else {
            value = (value * 2.0f) - 1.0f;
            value = (value >= 0)
                        ? lerp(slider->mid, slider->max, value)
                        : lerp(slider->mid, slider->min, -value);
        }

        if (value != slider->value) {
            slider->value = value;
            slider->changed = true;
        }
    }

    if (sliders[0].changed) {
        sliders[0].changed = false;
        SDL_SetAudioStreamFrequencyRatio(src_stream, sliders[0].value);
    }

    if (sliders[1].changed || sliders[2].changed) {
        sliders[1].changed = false;
        sliders[2].changed = false;
        spec.freq = (int)sliders[1].value;
        spec.channels = (int)sliders[2].value;
        SDL_SetAudioStreamFormat(tmp_stream, &spec, NULL);
    }

    if (sliders[3].changed || sliders[4].changed || sliders[5].changed) {
        sliders[3].changed = false;
        sliders[4].changed = false;
        sliders[5].changed = false;

        int quality = (int)sliders[3].value;
        float lpf = sliders[4].value;
        float db = sliders[5].value;

        SDL_AudioResampler *new_resampler = SDL_CreateAudioResampler(quality, lpf, db);
        SDL_SetAudioStreamResampler(src_stream, new_resampler);

        if (src_resampler) {
            SDL_DestroyAudioResampler(src_resampler);
        }

        src_resampler = new_resampler;
    }

    if (sliders[6].changed || sliders[7].changed || sliders[8].changed) {
        sliders[6].changed = false;
        sliders[7].changed = false;
        sliders[8].changed = false;

        int quality = (int)sliders[6].value;
        float lpf = sliders[7].value;
        float db = sliders[8].value;

        SDL_AudioResampler *new_resampler = SDL_CreateAudioResampler(quality, lpf, db);
        SDL_SetAudioStreamResampler(tmp_stream, new_resampler);

        if (tmp_resampler) {
            SDL_DestroyAudioResampler(tmp_resampler);
        }

        tmp_resampler = new_resampler;
    }

    if (SDL_GetAudioStreamFormat(src_stream, &src_spec, &tmp_spec)) {
        available_bytes = SDL_GetAudioStreamAvailable(src_stream);
        available_seconds = (float)available_bytes / (float)(SDL_AUDIO_FRAMESIZE(tmp_spec) * tmp_spec.freq);
    }

    if (SDL_GetAudioStreamFormat(dst_stream, NULL, &dst_spec)) {
    }

    queued_bytes = SDL_GetAudioStreamQueued(src_stream);

    for (i = 0; i < state->num_windows; i++) {
        int draw_y = 0;
        SDL_Renderer *rend = state->renderers[i];

        SDL_SetRenderDrawColor(rend, 0x00, 0x2B, 0x36, 0xFF);
        SDL_RenderClear(rend);

        for (j = 0; j < NUM_SLIDERS; ++j) {
            Slider *slider = &sliders[j];
            SDL_FRect area;

            SDL_copyp(&area, &slider->area);

            SDL_SetRenderDrawColor(rend, 0x07, 0x36, 0x42, 0xFF);
            SDL_RenderFillRect(rend, &area);

            area.w *= slider->pos;

            SDL_SetRenderDrawColor(rend, 0x58, 0x6E, 0x75, 0xFF);
            SDL_RenderFillRect(rend, &area);

            draw_textf(rend, (int)slider->area.x, (int)slider->area.y, slider->fmtlabel,
                       (slider->flags & 2) ? ((float)(int)slider->value) : slider->value);
        }

        draw_textf(rend, 0, draw_y, "%7s, Loop: %3s, Flush: %3s",
                   SDL_AudioDevicePaused(state->audio_id) ? "Paused" : "Playing", auto_loop ? "On" : "Off", auto_flush ? "On" : "Off");
        draw_y += FONT_LINE_HEIGHT;

        draw_textf(rend, 0, draw_y, "Available: %4.2f (%i bytes)", available_seconds, available_bytes);
        draw_y += FONT_LINE_HEIGHT;

        draw_textf(rend, 0, draw_y, "Queued: %i bytes", queued_bytes);
        draw_y += FONT_LINE_HEIGHT;

        SDL_LockAudioStream(dst_stream);

        draw_textf(rend, 0, draw_y, "Get Callback: %i/%i bytes, %2i ms ago, in %4i ns",
                   last_get_amount_additional, last_get_amount_total, (int)(SDL_GetTicks() - last_get_callback), last_get_timing);
        draw_y += FONT_LINE_HEIGHT;

        SDL_UnlockAudioStream(dst_stream);

        draw_y = state->window_h - FONT_LINE_HEIGHT * 3;

        draw_textf(rend, 0, draw_y, "Src: %6s/%6s/%i",
                   SDL_GetAudioFormatName(src_spec.format), AudioChansToStr(src_spec.channels), src_spec.freq);

        draw_y += FONT_LINE_HEIGHT;
        draw_textf(rend, 0, draw_y, "Tmp: %6s/%6s/%i",
                   SDL_GetAudioFormatName(spec.format), AudioChansToStr(spec.channels), spec.freq);
        draw_y += FONT_LINE_HEIGHT;

        draw_textf(rend, 0, draw_y, "Dst: %6s/%6s/%i",
                   SDL_GetAudioFormatName(dst_spec.format), AudioChansToStr(dst_spec.channels), dst_spec.freq);
        draw_y += FONT_LINE_HEIGHT;

        SDL_RenderPresent(rend);
    }
}

static void SDLCALL src_get_callback(void *userdata, SDL_AudioStream *strm, int additional_amount, int total_amount)
{
    if (auto_loop && (additional_amount > 0)) {
        while (additional_amount > 0) {
            SDL_PutAudioStreamDataNoCopy(strm, audio_buf, audio_len, NULL, NULL);
            additional_amount -= audio_len;
        }

        if (auto_flush) {
            SDL_FlushAudioStream(strm);
        }
    }
}

static void SDLCALL tmp_get_callback(void *userdata, SDL_AudioStream *strm, int additional_amount, int total_amount)
{
    if (additional_amount > 0) {
        SDL_AudioSpec cvt_spec;
        SDL_GetAudioStreamFormat(strm, &cvt_spec, NULL);
        SDL_SetAudioStreamFormat(src_stream, NULL, &cvt_spec);

        while (additional_amount > 0) {
            char buffer[4096];
            int length = SDL_GetAudioStreamData(src_stream, buffer, SDL_min(additional_amount, sizeof(buffer)));

            if (length <= 0)
                break;

            SDL_PutAudioStreamData(strm, buffer, length);
            additional_amount -= length;
        }
    }
}

static void SDLCALL dst_get_callback(void *userdata, SDL_AudioStream *strm, int additional_amount, int total_amount)
{
    last_get_callback = SDL_GetTicks();
    last_get_amount_additional = additional_amount;
    last_get_amount_total = total_amount;
    Uint64 elapsed_time = SDL_GetTicksNS();

    if (additional_amount > 0) {
        while (additional_amount > 0) {
            char buffer[4096];
            int length = SDL_GetAudioStreamData(tmp_stream, buffer, SDL_min(additional_amount, sizeof(buffer)));

            if (length <= 0)
                break;

            SDL_PutAudioStreamData(strm, buffer, length);
            additional_amount -= length;
        }
    }

    last_get_timing = SDL_GetTicksNS() - elapsed_time;
}

int main(int argc, char *argv[])
{
    char *filename = NULL;
    int i;

    /* Initialize test framework */
    state = SDLTest_CommonCreateState(argv, SDL_INIT_AUDIO | SDL_INIT_VIDEO);
    if (!state) {
        return 1;
    }

    /* Parse commandline */
    for (i = 1; i < argc;) {
        int consumed;

        consumed = SDLTest_CommonArg(state, i);
        if (!consumed) {
            if (!filename) {
                filename = argv[i];
                consumed = 1;
            }
        }
        if (consumed <= 0) {
            static const char *options[] = { "[sample.wav]", NULL };
            SDLTest_CommonLogUsage(state, argv[0], options);
            exit(1);
        }

        i += consumed;
    }

    /* Load the SDL library */
    if (!SDLTest_CommonInit(state)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL: %s", SDL_GetError());
        return 1;
    }

    FONT_CHARACTER_SIZE = 16;

    filename = GetResourceFilename(filename, "sample.wav");

    SDL_AudioSpec src_spec;

    if (!SDL_LoadWAV(filename, &src_spec, &audio_buf, &audio_len)) {
        SDL_Log("Failed to load '%s': %s", filename, SDL_GetError());
        SDL_free(filename);
        SDL_Quit();
        return 1;
    }

    SDL_copyp(&spec, &src_spec);
    spec.format = SDL_AUDIO_F32;

    SDL_free(filename);
    init_slider(0, "Speed: %3.2fx", 0x0, 1.0f, 0.2f, 5.0f);
    init_slider(1, "Freq: %g", 0x6, (float)spec.freq, 4000.0f, 192000.0f);
    init_slider(2, "Channels: %g", 0x3, (float)spec.channels, 1.0f, 8.0f);
    init_slider(3, "Src Quality: %g", 0x3, 0.0f, -1.0f, 5.0f);
    init_slider(4, "Src Rolloff: %3.2f", 0x4, 1.0f, 0.0f, 1.0f);
    init_slider(5, "Src Rolloff dB: %2.0f", 0x4, 80.0, 21.0f, 120.0f);
    init_slider(6, "Dst Quality: %g", 0x3, 0.0f, -1.0f, 5.0f);
    init_slider(7, "Dst Rolloff: %3.2f", 0x4, 1.0f, 0.0f, 1.0f);
    init_slider(8, "Dst Rolloff dB: %2.0f", 0x4, 80.0, 21.0f, 120.0f);

    for (i = 0; i < state->num_windows; i++) {
        SDL_SetWindowTitle(state->windows[i], "Resampler Test");
    }

    SDL_AudioSpec dst_spec;
    if (SDL_GetAudioDeviceFormat(state->audio_id, &dst_spec, NULL)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Device Spec: %6s/%6s/%i",
                    SDL_GetAudioFormatName(spec.format), AudioChansToStr(spec.channels), spec.freq);
    } else {
        SDL_copyp(&dst_spec, &spec);
    }

    src_stream = SDL_CreateAudioStream(&src_spec, &spec);
    tmp_stream = SDL_CreateAudioStream(&spec, &dst_spec);
    dst_stream = SDL_CreateAudioStream(&dst_spec, &dst_spec);
    SDL_SetAudioStreamGetCallback(src_stream, src_get_callback, NULL);
    SDL_SetAudioStreamGetCallback(tmp_stream, tmp_get_callback, NULL);
    SDL_SetAudioStreamGetCallback(dst_stream, dst_get_callback, NULL);
    SDL_BindAudioStream(state->audio_id, dst_stream);

#ifdef SDL_PLATFORM_EMSCRIPTEN
    emscripten_set_main_loop(loop, 0, 1);
#else
    while (!done) {
        loop();
    }
#endif

    SDLTest_CleanupTextDrawing();
    SDL_DestroyAudioStream(dst_stream);
    SDL_DestroyAudioStream(tmp_stream);
    SDL_DestroyAudioStream(src_stream);
    SDL_DestroyAudioResampler(src_resampler);
    SDL_DestroyAudioResampler(tmp_resampler);
    SDL_free(audio_buf);
    SDLTest_CommonQuit(state);
    return 0;
}
