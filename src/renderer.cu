/*
 * renderer.cu — CUDA კომპოზიტინგი + FFmpeg/NVENC მილი.
 *
 * კადრის სასიცოცხლო გზა (ორმაგი ბუფერიზაციით):
 *
 *   [CPU]  ტაიმლაინის შეფასება (არენაში)  →  ვიჯეტების runtime მდგომარეობა
 *     ↓                                        (opacity, პოზიცია, კუთხე, reveal)
 *   [H2D]  აკრეფის ზღვრები → VRAM          →  სტრიქონზე თითო float
 *     ↓
 *   [GPU]  k_clear_background                →  მთელი კადრი ერთი ფერით
 *     ↓
 *   [GPU]  k_composite_texture × N           →  შრეების დაფენა ინვერსიული
 *     ↓                                        მატრიცით (მასშტაბი + ბრუნვა)
 *   [GPU]  k_rgba_to_nv12                    →  8.29 MB → 3.11 MB (1080x1920)
 *     ↓
 *   [D2H]  cudaMemcpyAsync → pinned host RAM
 *     ↓
 *   [PIPE] fwrite → ffmpeg -c:v h264_nvenc   →  .mp4
 *
 * ორმაგი ბუფერიზაცია: სანამ CPU კადრ i-1-ს მილში წერს (და NVENC მას აკოდირებს),
 * GPU უკვე კადრ i-ს არენდერებს.
 *
 * გაზომვის შედეგი: ამ მილში ვიწრო ყელი *არც* კომპოზიტინგია და *არც* კოდირება,
 * არამედ კადრის host-ში გადატანა და მილში ჩაწერა. სწორედ ამიტომ NV12-ად
 * გარდაქმნა (და არა ორმაგი ბუფერიზაცია) იძლევა რეალურ მოგებას — ის უშუალოდ
 * ამცირებს გადასატან ბაიტებს.
 */

#include <cuda_runtime.h>

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "renderer.h"
#include "arena.h"
#include "audio.h"
#include "effects.h"

/* ------------------------------------------------------------------------- */
/* CUDA-ს შეცდომების მკაცრი შემოწმება                                         */
/* ------------------------------------------------------------------------- */

/*
 * ორი დონე, განზრახ:
 *
 *   gpuErrchk(call)      — პროგრამისტის შეცდომაზე (არასწორი launch config,
 *                          გატეხილი მაჩვენებელი) მაშინვე ვწყვეტთ.
 *
 *   CUDA_TRY(call)       — მოსალოდნელ, გარემოზე დამოკიდებულ ჩავარდნაზე
 *                          (VRAM არ ჰყოფნის) ვბრუნდებით false-ით, რომ
 *                          renderer_shutdown()-მა ყველაფერი დაალაგოს.
 */
static void gpu_assert(cudaError_t code, const char *file, int line, bool abort_on_error)
{
    if (code == cudaSuccess) {
        return;
    }
    fprintf(stderr, "\nCUDA შეცდომა: %s (%s)\n  → %s:%d\n",
            cudaGetErrorString(code), cudaGetErrorName(code), file, line);
    if (abort_on_error) {
        exit(EXIT_FAILURE);
    }
}

#define gpuErrchk(ans) gpu_assert((ans), __FILE__, __LINE__, true)

#define CUDA_TRY(ans)                                          \
    do {                                                       \
        cudaError_t err_ = (ans);                              \
        if (err_ != cudaSuccess) {                             \
            gpu_assert(err_, __FILE__, __LINE__, false);       \
            return false;                                      \
        }                                                      \
    } while (0)

#define CUDA_CHECK_KERNEL() gpuErrchk(cudaGetLastError())

/* ------------------------------------------------------------------------- */
/* Device რესურსები (გაუმჭვირვალე EditorContext::gpu-ს მიღმა)                 */
/* ------------------------------------------------------------------------- */

/* ერთი "სლოტი" = ერთი კადრი მილში. */
struct FrameSlot {
    uchar4      *d_frame;   /* RGBA კადრის ბუფერი VRAM-ში (კომპოზიტინგისთვის) */
    uchar4      *d_fx;      /* ping-pong ბუფერი ეფექტებისთვის                  */
    uchar4      *d_scene[2];/* ორი სცენა გადასვლის დროს; NULL თუ სცენა ერთია   */
    uint8_t     *d_nv12;    /* იგივე კადრი NV12-ად — მხოლოდ ეს მიდის host-ში   */
    uint8_t     *h_frame;   /* pinned host staging — სწრაფი DMA-სთვის          */
    cudaStream_t stream;
    cudaEvent_t  done;      /* ინიშნება D2H-ის დასრულების შემდეგ               */
};

struct RenderResources {
    FrameSlot slot[VR_PIPELINE_DEPTH];
    size_t    frame_bytes;  /* RGBA framebuffer-ის ზომა (VRAM-ში)              */
    size_t    nv12_bytes;   /* w*h*3/2 — რეალურად გადაცემული მოცულობა          */
    int       width;
    int       height;
    size_t    texture_bytes;
    int       texture_count;
};

/* ------------------------------------------------------------------------- */
/* KERNELS                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * ფონის შევსება.
 *
 * ერთი thread = ერთი პიქსელი. 16x16 ბლოკი (256 thread) კარგად ჯდება
 * warp-ის ჯერადში და მეზობელ thread-ებს მეხსიერების მიმდევრობით (coalesced)
 * წვდომას აძლევს — მთელი მწკრივი ერთ ტრანზაქციად იწერება.
 */
__global__ void k_clear_background(uchar4 *__restrict__ fb, int width, int height, uchar4 bg)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || y >= height) {
        return; /* კიდის ბლოკები ყოველთვის ნაწილობრივ "ცარიელია" */
    }
    fb[(size_t)y * width + x] = bg;
}

/* ბილინეარული შერჩევა premultiplied ტექსტურიდან; u,v — უწყვეტი კოორდინატები
 * პიქსელის ერთეულებში (ტექსელის ცენტრი i+0.5-ზეა). */
__device__ __forceinline__ float4 sample_bilinear(const uchar4 *__restrict__ tex,
                                                  int tw, int th, float u, float v)
{
    float fx = u - 0.5f;
    float fy = v - 0.5f;

    int   x0 = (int)floorf(fx);
    int   y0 = (int)floorf(fy);
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;

    int x1 = x0 + 1;
    int y1 = y0 + 1;

    /* კიდეების clamp — ალტერნატივა (wrap) ტექსტს მეორე მხრიდან შემოიტანდა. */
    x0 = min(max(x0, 0), tw - 1);
    x1 = min(max(x1, 0), tw - 1);
    y0 = min(max(y0, 0), th - 1);
    y1 = min(max(y1, 0), th - 1);

    uchar4 p00 = tex[(size_t)y0 * tw + x0];
    uchar4 p10 = tex[(size_t)y0 * tw + x1];
    uchar4 p01 = tex[(size_t)y1 * tw + x0];
    uchar4 p11 = tex[(size_t)y1 * tw + x1];

    const float inv = 1.0f / 255.0f;
    float w00 = (1.0f - tx) * (1.0f - ty);
    float w10 = tx * (1.0f - ty);
    float w01 = (1.0f - tx) * ty;
    float w11 = tx * ty;

    /* ინტერპოლაცია premultiplied სივრცეში — მხოლოდ ასეა სწორი ნახევრად-
     * გამჭვირვალე კიდეებზე (straight alpha-ზე კონტურები "ბინძურდება"). */
    float4 out;
    out.x = (p00.x * w00 + p10.x * w10 + p01.x * w01 + p11.x * w11) * inv;
    out.y = (p00.y * w00 + p10.y * w10 + p01.y * w01 + p11.y * w11) * inv;
    out.z = (p00.z * w00 + p10.z * w10 + p01.z * w01 + p11.z * w11) * inv;
    out.w = (p00.w * w00 + p10.w * w10 + p01.w * w01 + p11.w * w11) * inv;
    return out;
}

/*
 * RGBA → NV12 გარდაქმნა GPU-ზე.
 *
 * რატომ არის ეს ყველაზე მნიშვნელოვანი ოპტიმიზაცია მთელ მილში:
 *
 *   დაუმუშავებელი RGBA კადრი 1080x1920-ზე არის 8.29 MB. NV12 (4:2:0) იგივე
 *   კადრს 3.11 MB-ში ინახავს — 2.67-ჯერ ნაკლები. ეს მოცულობა ორჯერ იზოგება:
 *   ჯერ D2H გადაცემისას PCIe-ზე, მერე ffmpeg-ის მილში ჩაწერისას.
 *
 *   და რაც მთავარია — ადრე ამ გარდაქმნას (RGBA→yuv420p) *ffmpeg აკეთებდა CPU-ზე*,
 *   ყოველ კადრზე, რამდენიმე ნაკადში. ახლა მას GPU აკეთებს პრაქტიკულად უფასოდ,
 *   იმ მონაცემებზე, რომლებიც ისედაც VRAM-შია.
 *
 * ხარისხი არ იკარგება: საბოლოო ფაილი ისედაც yuv420p იყო — უბრალოდ გარდაქმნა
 * სხვა ადგილას კეთდება.
 *
 * ერთი thread ამუშავებს 2x2 ბლოკს: წერს 4 Y-ს და ერთ საშუალო UV წყვილს.
 * კოეფიციენტები — BT.709 limited range ("studio swing"), HD ვიდეოს სტანდარტი.
 */
__global__ void k_rgba_to_nv12(const uchar4 *__restrict__ rgba,
                               uint8_t *__restrict__ y_plane,
                               uint8_t *__restrict__ uv_plane,
                               int width, int height)
{
    int bx = blockIdx.x * blockDim.x + threadIdx.x; /* 2x2 ბლოკის ინდექსი */
    int by = blockIdx.y * blockDim.y + threadIdx.y;

    int x = bx * 2;
    int y = by * 2;

    if (x >= width || y >= height) {
        return;
    }

    /* ოთხივე პიქსელი; კიდეზე (კენტი ზომა) ვიმეორებთ ბოლოს. */
    int x1 = min(x + 1, width - 1);
    int y1 = min(y + 1, height - 1);

    uchar4 p00 = rgba[(size_t)y  * width + x];
    uchar4 p10 = rgba[(size_t)y  * width + x1];
    uchar4 p01 = rgba[(size_t)y1 * width + x];
    uchar4 p11 = rgba[(size_t)y1 * width + x1];

    /* --- Y (სრული გარჩევადობით) ------------------------------------------ */
    #define VR_LUMA(p) (16.0f + 0.18259f * (p).x + 0.61423f * (p).y + 0.06201f * (p).z)

    y_plane[(size_t)y  * width + x ] = (uint8_t)(__saturatef(VR_LUMA(p00) / 255.0f) * 255.0f + 0.5f);
    if (x + 1 < width) {
        y_plane[(size_t)y * width + x + 1] =
            (uint8_t)(__saturatef(VR_LUMA(p10) / 255.0f) * 255.0f + 0.5f);
    }
    if (y + 1 < height) {
        y_plane[(size_t)(y + 1) * width + x] =
            (uint8_t)(__saturatef(VR_LUMA(p01) / 255.0f) * 255.0f + 0.5f);
        if (x + 1 < width) {
            y_plane[(size_t)(y + 1) * width + x + 1] =
                (uint8_t)(__saturatef(VR_LUMA(p11) / 255.0f) * 255.0f + 0.5f);
        }
    }
    #undef VR_LUMA

    /* --- UV (ნახევარ გარჩევადობაზე, 2x2 ბლოკის საშუალო) ------------------- */
    float r = (p00.x + p10.x + p01.x + p11.x) * 0.25f;
    float g = (p00.y + p10.y + p01.y + p11.y) * 0.25f;
    float b = (p00.z + p10.z + p01.z + p11.z) * 0.25f;

    float u = 128.0f - 0.10064f * r - 0.33857f * g + 0.43922f * b;
    float v = 128.0f + 0.43922f * r - 0.39894f * g - 0.04027f * b;

    size_t uv_idx = ((size_t)by * ((width + 1) / 2) + bx) * 2;
    uv_plane[uv_idx    ] = (uint8_t)(__saturatef(u / 255.0f) * 255.0f + 0.5f);
    uv_plane[uv_idx + 1] = (uint8_t)(__saturatef(v / 255.0f) * 255.0f + 0.5f);
}


/* ========================================================================= */
/* POST-PROCESSING ეფექტები                                                  */
/* ========================================================================= */

/*
 * ეფექტების GPU-ს მხარე.
 *
 * host-ი ყოველ კადრზე Track-ებს "ასინჯავს" და მიღებულ რიცხვებს ამ POD
 * სტრუქტურაში აწყობს — ანუ კერნელს არასოდეს უწევს keyframe-ების გარჩევა.
 */
struct EffectGPU {
    int   type;
    float p[FXP_MAX];
    float ca[4];   /* color_a 0..1 */
    float cb[4];   /* color_b 0..1 */
};

__device__ __forceinline__ float3 fx_load(const uchar4 &c)
{
    const float inv = 1.0f / 255.0f;
    return make_float3(c.x * inv, c.y * inv, c.z * inv);
}

__device__ __forceinline__ uchar4 fx_store(float3 c, unsigned char a)
{
    return make_uchar4((unsigned char)(__saturatef(c.x) * 255.0f + 0.5f),
                       (unsigned char)(__saturatef(c.y) * 255.0f + 0.5f),
                       (unsigned char)(__saturatef(c.z) * 255.0f + 0.5f), a);
}

/* BT.709-ის ნათელობა — იგივე წონები, რაც NV12-ის გარდაქმნაში. */
__device__ __forceinline__ float fx_luma(float3 c)
{
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

__device__ __forceinline__ float3 fx_mix(float3 a, float3 b, float t)
{
    return make_float3(a.x + (b.x - a.x) * t,
                       a.y + (b.y - a.y) * t,
                       a.z + (b.z - a.z) * t);
}

/*
 * მთელი რიცხვის hash → [0,1) ფსევდო-შემთხვევითი.
 *
 * ცალკე RNG-ის მდგომარეობა არ გვჭირდება: მარცვალს პიქსელის კოორდინატიდან და
 * კადრის ნომრიდან ვაწარმოებთ, ამიტომ შედეგი დეტერმინისტულია (ერთი და იგივე
 * კადრი ყოველთვის ერთნაირად "მარცვლოვნდება"), მაგრამ კადრებს შორის იცვლება.
 */
__device__ __forceinline__ float fx_hash(unsigned int x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return (float)(x & 0x00FFFFFFU) / (float)0x01000000U;
}

/* ერთი ეფექტის გამოთვლა ერთ პიქსელზე (მეზობლების გარეშე). */
__device__ float3 fx_apply_point(float3 c, const EffectGPU &fx,
                                 int x, int y, int w, int h, unsigned int seed)
{
    switch (fx.type) {
        case FX_GRAYSCALE: {
            float a = __saturatef(fx.p[FXP_AMOUNT]);
            float l = fx_luma(c);
            return fx_mix(c, make_float3(l, l, l), a);
        }
        case FX_INVERT: {
            float a = __saturatef(fx.p[FXP_AMOUNT]);
            return fx_mix(c, make_float3(1.0f - c.x, 1.0f - c.y, 1.0f - c.z), a);
        }
        case FX_SEPIA: {
            float a = __saturatef(fx.p[FXP_AMOUNT]);
            float3 s = make_float3(c.x * 0.393f + c.y * 0.769f + c.z * 0.189f,
                                   c.x * 0.349f + c.y * 0.686f + c.z * 0.168f,
                                   c.x * 0.272f + c.y * 0.534f + c.z * 0.131f);
            return fx_mix(c, s, a);
        }
        case FX_POSTERIZE: {
            float n = fmaxf(2.0f, fx.p[FXP_LEVELS]);
            return make_float3(floorf(c.x * n) / (n - 1.0f),
                               floorf(c.y * n) / (n - 1.0f),
                               floorf(c.z * n) / (n - 1.0f));
        }
        case FX_THRESHOLD: {
            float t = fx.p[FXP_LEVEL];
            float v = (fx_luma(c) >= t) ? 1.0f : 0.0f;
            return make_float3(v, v, v);
        }
        case FX_VIGNETTE: {
            /* ნორმალიზებული მანძილი ცენტრიდან, გვერდითი თანაფარდობის გათვალისწინებით. */
            float nx = ((float)x + 0.5f) / (float)w - 0.5f;
            float ny = ((float)y + 0.5f) / (float)h - 0.5f;
            float d  = sqrtf(nx * nx + ny * ny) * 1.41421356f;

            float r    = fmaxf(0.01f, fx.p[FXP_RADIUS]);
            float soft = fmaxf(0.01f, fx.p[FXP_SOFTNESS]);
            float t    = __saturatef((d - r) / soft);
            t = t * t * (3.0f - 2.0f * t);           /* smoothstep */

            float3 tint = make_float3(fx.ca[0], fx.ca[1], fx.ca[2]);
            return fx_mix(c, tint, t * __saturatef(fx.p[FXP_AMOUNT]));
        }
        case FX_GRAIN: {
            float a = fx.p[FXP_AMOUNT];
            float n = fx_hash((unsigned int)(y * w + x) * 2654435761U + seed) - 0.5f;
            return make_float3(c.x + n * a, c.y + n * a, c.z + n * a);
        }
        case FX_SCANLINES: {
            float a     = __saturatef(fx.p[FXP_AMOUNT]);
            float count = fmaxf(1.0f, fx.p[FXP_COUNT]);
            float s     = sinf((float)y / (float)h * count * 3.14159265f);
            float k     = 1.0f - a * 0.5f * (1.0f - s * s);
            return make_float3(c.x * k, c.y * k, c.z * k);
        }
        case FX_VIBRANCE: {
            /* ნაკლებად ნაჯერ ფერებს უფრო აძლიერებს — კანის ტონები დაცულია. */
            float a   = fx.p[FXP_AMOUNT];
            float mx  = fmaxf(c.x, fmaxf(c.y, c.z));
            float mn  = fminf(c.x, fminf(c.y, c.z));
            float sat = mx - mn;
            float l   = fx_luma(c);
            return fx_mix(make_float3(l, l, l), c, 1.0f + a * (1.0f - sat));
        }
        case FX_SPLIT_TONE: {
            /* ჩრდილებს ერთ ტონს ვაძლევთ, შუქებს — მეორეს (teal/orange). */
            float l   = fx_luma(c);
            float bal = fx.p[FXP_BALANCE];
            float t   = __saturatef((l - bal) * 2.0f + 0.5f);
            float3 sh = make_float3(fx.ca[0], fx.ca[1], fx.ca[2]);
            float3 hi = make_float3(fx.cb[0], fx.cb[1], fx.cb[2]);
            float3 tone = fx_mix(sh, hi, t);
            /* soft-light-ისებური შერევა, რომ ნათელობა შენარჩუნდეს */
            float3 blended = make_float3(c.x * (0.5f + tone.x), c.y * (0.5f + tone.y),
                                         c.z * (0.5f + tone.z));
            return fx_mix(c, blended, __saturatef(fx.p[FXP_AMOUNT]));
        }
        case FX_GRADIENT_MAP: {
            float l = fx_luma(c);
            float3 sh = make_float3(fx.ca[0], fx.ca[1], fx.ca[2]);
            float3 hi = make_float3(fx.cb[0], fx.cb[1], fx.cb[2]);
            return fx_mix(c, fx_mix(sh, hi, l), __saturatef(fx.p[FXP_AMOUNT]));
        }
        case FX_COLOR_GRADE: {
            /* რიგი კოლორისტის მუშაობას იმეორებს: ექსპოზიცია → კონტრასტი →
             * გამა → ტემპერატურა → ნაჯერობა → ელფერი. */
            float e = fx.p[FXP_EXPOSURE];
            if (e != 0.0f) {
                float k = exp2f(e);
                c = make_float3(c.x * k, c.y * k, c.z * k);
            }
            float br = fx.p[FXP_BRIGHTNESS];
            if (br != 0.0f) {
                c = make_float3(c.x + br, c.y + br, c.z + br);
            }
            float ct = fx.p[FXP_CONTRAST];
            if (ct != 1.0f) {
                c = make_float3((c.x - 0.5f) * ct + 0.5f,
                                (c.y - 0.5f) * ct + 0.5f,
                                (c.z - 0.5f) * ct + 0.5f);
            }
            float gm = fx.p[FXP_GAMMA];
            if (gm > 0.0f && gm != 1.0f) {
                float ig = 1.0f / gm;
                c = make_float3(powf(fmaxf(c.x, 0.0f), ig),
                                powf(fmaxf(c.y, 0.0f), ig),
                                powf(fmaxf(c.z, 0.0f), ig));
            }
            float tmp = fx.p[FXP_TEMPERATURE];
            float tnt = fx.p[FXP_TINT];
            if (tmp != 0.0f || tnt != 0.0f) {
                c = make_float3(c.x + tmp * 0.2f, c.y + tnt * 0.2f, c.z - tmp * 0.2f);
            }
            float sa = fx.p[FXP_SATURATION];
            if (sa != 1.0f) {
                float l = fx_luma(c);
                c = fx_mix(make_float3(l, l, l), c, sa);
            }
            float vb = fx.p[FXP_VIBRANCE];
            if (vb != 0.0f) {
                float mx = fmaxf(c.x, fmaxf(c.y, c.z));
                float mn = fminf(c.x, fminf(c.y, c.z));
                float l  = fx_luma(c);
                c = fx_mix(make_float3(l, l, l), c, 1.0f + vb * (1.0f - (mx - mn)));
            }
            float hu = fx.p[FXP_HUE];
            if (hu != 0.0f) {
                /* ბრუნვა ელფერის ღერძის ირგვლივ (YIQ-ის მიახლოება). */
                float a  = hu * 3.14159265f / 180.0f;
                float cs = cosf(a), sn = sinf(a);
                float3 o = c;
                c.x = o.x * (0.299f + 0.701f * cs + 0.168f * sn) +
                      o.y * (0.587f - 0.587f * cs + 0.330f * sn) +
                      o.z * (0.114f - 0.114f * cs - 0.497f * sn);
                c.y = o.x * (0.299f - 0.299f * cs - 0.328f * sn) +
                      o.y * (0.587f + 0.413f * cs + 0.035f * sn) +
                      o.z * (0.114f - 0.114f * cs + 0.292f * sn);
                c.z = o.x * (0.299f - 0.300f * cs + 1.250f * sn) +
                      o.y * (0.587f - 0.588f * cs - 1.050f * sn) +
                      o.z * (0.114f + 0.886f * cs - 0.203f * sn);
            }
            return c;
        }
        default:
            return c;
    }
}

/* ერთპიქსელიანი ეფექტების კერნელი — ერთ გავლაში რამდენიმე ეფექტს ასრულებს. */
__global__ void k_fx_point(uchar4 *__restrict__ dst, const uchar4 *__restrict__ src,
                           int w, int h, EffectGPU fx, unsigned int seed)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }

    size_t i = (size_t)y * w + x;
    uchar4 s = src[i];
    dst[i]   = fx_store(fx_apply_point(fx_load(s), fx, x, y, w, h, seed), s.w);
}

/*
 * გამყოფადი ბუნდოვნება.
 *
 * ორი ერთგანზომილებიანი გავლა O(2r) ნიმუშს კითხულობს პიქსელზე, ერთი
 * ორგანზომილებიანი კი O(r²)-ს. r=20-ზე ეს 40 vs 1600 — ორმოცჯერ ნაკლები.
 */
__global__ void k_fx_blur(uchar4 *__restrict__ dst, const uchar4 *__restrict__ src,
                          int w, int h, int radius, int horizontal)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }

    float3 sum = make_float3(0.0f, 0.0f, 0.0f);
    float  wsum = 0.0f;

    for (int k = -radius; k <= radius; k++) {
        int sx = horizontal ? min(max(x + k, 0), w - 1) : x;
        int sy = horizontal ? y : min(max(y + k, 0), h - 1);

        /* სამკუთხა წონები — იაფი მიახლოება გაუსისა, ორი გავლის შემდეგ
         * პრაქტიკულად გაუსური ბირთვი გამოდის. */
        float wt = (float)(radius + 1 - abs(k));
        float3 c = fx_load(src[(size_t)sy * w + sx]);
        sum.x += c.x * wt; sum.y += c.y * wt; sum.z += c.z * wt;
        wsum  += wt;
    }

    size_t i = (size_t)y * w + x;
    float inv = (wsum > 0.0f) ? 1.0f / wsum : 1.0f;
    dst[i] = fx_store(make_float3(sum.x * inv, sum.y * inv, sum.z * inv), src[i].w);
}

__global__ void k_fx_pixelate(uchar4 *__restrict__ dst, const uchar4 *__restrict__ src,
                              int w, int h, int size)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    if (size < 1) {
        size = 1;
    }

    /* ბლოკის ცენტრიდან ვიღებთ ნიმუშს — ბლოკის საშუალოსთან ვიზუალურად ახლოა,
     * მაგრამ size² კითხვის ნაცვლად ერთი კითხვაა. */
    int bx = (x / size) * size + size / 2;
    int by = (y / size) * size + size / 2;
    bx = min(bx, w - 1);
    by = min(by, h - 1);

    dst[(size_t)y * w + x] = src[(size_t)by * w + bx];
}

/* ქრომატული აბერაცია — არხები ერთმანეთისგან იშლება. */
__global__ void k_fx_rgb_split(uchar4 *__restrict__ dst, const uchar4 *__restrict__ src,
                               int w, int h, float amount, float angle_deg)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }

    float a  = angle_deg * 3.14159265f / 180.0f;
    float dx = cosf(a) * amount;
    float dy = sinf(a) * amount;

    int rx = min(max((int)lrintf((float)x + dx), 0), w - 1);
    int ry = min(max((int)lrintf((float)y + dy), 0), h - 1);
    int bx = min(max((int)lrintf((float)x - dx), 0), w - 1);
    int by = min(max((int)lrintf((float)y - dy), 0), h - 1);

    size_t i = (size_t)y * w + x;
    dst[i] = make_uchar4(src[(size_t)ry * w + rx].x,
                         src[i].y,
                         src[(size_t)by * w + bx].z,
                         src[i].w);
}

/* ჰორიზონტალური ზოლების შემთხვევითი წანაცვლება + არხების გაცალკევება. */
__global__ void k_fx_glitch(uchar4 *__restrict__ dst, const uchar4 *__restrict__ src,
                            int w, int h, float amount, unsigned int seed)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }

    int   band = y / 12;                                  /* 12-პიქსელიანი ზოლები */
    float r    = fx_hash((unsigned int)band * 9781U + seed);

    int shift = 0;
    if (r > 0.75f) {                                      /* ზოლების მეოთხედი იშლება */
        shift = (int)((fx_hash((unsigned int)band * 6151U + seed) - 0.5f) * amount * w);
    }

    int sx = min(max(x + shift, 0), w - 1);
    size_t i = (size_t)y * w + x;

    uchar4 c = src[(size_t)y * w + sx];
    if (r > 0.9f) {
        int gx = min(max(sx + (int)(amount * 12.0f), 0), w - 1);
        c.y = src[(size_t)y * w + gx].y;
    }
    dst[i] = c;
}


/* ------------------------------------------------------------------------- */
/* გადასვლის კომპოზიტორი                                                      */
/* ------------------------------------------------------------------------- */

/*
 * ორი უკვე დარენდერებული სცენა ერთ კადრად.
 *
 * თითოეულ მხარეს აქვს ინვერსიული გარდაქმნა (გადატანა + მასშტაბი + ბრუნვა) და
 * არჩევითი მასკა. კერნელი უკუპროექციით მუშაობს, ისევე როგორც ტექსტურების
 * კომპოზიტორი: ყოველი *გამომავალი* პიქსელისთვის ვეძებთ, საიდან უნდა წაიკითხოს.
 *
 * `from` ჯერ იხატება, `to` — ზემოდან. ამიტომ crossfade უბრალოდ `to`-ს გამჭვირვალობის
 * ზრდაა, slide კი მისი გადმოწევა.
 */
struct TransSide {
    float opacity;
    float ia, ib, ic, id;   /* ინვერსიული 2x2 */
    float tx, ty;           /* გადატანა პიქსელებში */
    int   mask;             /* 0 = არაა, 1 = წრე, 2 = მართკუთხედი */
    float m0, m1, m2, m3;   /* წრე: cx,cy,r | მართკუთხედი: x,y,w,h (კადრის წილადებში) */
};

struct TransParams {
    int       w, h;
    uchar4    bg;
    TransSide from, to;
};

__device__ __forceinline__ bool trans_mask_ok(const TransSide &s, float nx, float ny)
{
    if (s.mask == 1) {
        float dx = nx - s.m0, dy = ny - s.m1;
        return (dx * dx + dy * dy) <= (s.m2 * s.m2);
    }
    if (s.mask == 2) {
        return nx >= s.m0 && nx <= s.m0 + s.m2 && ny >= s.m1 && ny <= s.m1 + s.m3;
    }
    return true;
}

/* ერთი მხარის შერჩევა; აბრუნებს false-ს, თუ პიქსელს ეს სცენა არ ფარავს. */
__device__ __forceinline__ bool trans_sample(const uchar4 *__restrict__ src,
                                             const TransSide &s, int w, int h,
                                             float px, float py, float4 *out)
{
    if (s.opacity <= 0.0f) {
        return false;
    }

    float nx = px / (float)w, ny = py / (float)h;
    if (!trans_mask_ok(s, nx, ny)) {
        return false;
    }

    float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
    float dx = px - cx - s.tx;
    float dy = py - cy - s.ty;

    float u = cx + (s.ia * dx + s.ib * dy);
    float v = cy + (s.ic * dx + s.id * dy);

    if (u < 0.0f || u >= (float)w || v < 0.0f || v >= (float)h) {
        return false;   /* კადრს გარეთ გაწეული სცენა — ფონი ჩანს */
    }

    *out = sample_bilinear(src, w, h, u, v);
    return true;
}

__global__ void k_transition(uchar4 *__restrict__ dst,
                             const uchar4 *__restrict__ from,
                             const uchar4 *__restrict__ to,
                             TransParams p)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p.w || y >= p.h) {
        return;
    }

    float px = (float)x + 0.5f;
    float py = (float)y + 0.5f;

    const float inv = 1.0f / 255.0f;
    float3 acc = make_float3(p.bg.x * inv, p.bg.y * inv, p.bg.z * inv);

    float4 c;
    if (trans_sample(from, p.from, p.w, p.h, px, py, &c)) {
        float a = __saturatef(p.from.opacity);
        acc.x = c.x * a + acc.x * (1.0f - a);
        acc.y = c.y * a + acc.y * (1.0f - a);
        acc.z = c.z * a + acc.z * (1.0f - a);
    }
    if (trans_sample(to, p.to, p.w, p.h, px, py, &c)) {
        float a = __saturatef(p.to.opacity);
        acc.x = c.x * a + acc.x * (1.0f - a);
        acc.y = c.y * a + acc.y * (1.0f - a);
        acc.z = c.z * a + acc.z * (1.0f - a);
    }

    dst[(size_t)y * p.w + x] = make_uchar4(
        (unsigned char)(__saturatef(acc.x) * 255.0f + 0.5f),
        (unsigned char)(__saturatef(acc.y) * 255.0f + 0.5f),
        (unsigned char)(__saturatef(acc.z) * 255.0f + 0.5f), 255);
}

/*
 * კომპოზიტინგის პარამეტრები. სტრუქტურად იმიტომ, რომ არგუმენტების სია
 * აღარ იზრდებოდეს — CUDA მას ისედაც constant bank-ში აგზავნის.
 */
struct CompositeParams {
    int   fb_w, fb_h;
    int   tex_w, tex_h;

    /* დანიშნულების ცენტრი კადრის კოორდინატებში. */
    float cx, cy;

    /*
     * ინვერსიული 2x2 მატრიცა: კადრის სივრციდან ტექსტურის სივრცეში.
     *
     * პირდაპირი გარდაქმნაა  d = R(θ) · S · t  (ჯერ მასშტაბი, მერე ბრუნვა).
     * კერნელი კი უკუპროექციით მუშაობს — ყოველი *დანიშნულების* პიქსელისთვის
     * ვეძებთ, საიდან უნდა წაიკითხოს. ეს ხვრელების გარეშე გვაძლევს შედეგს
     * (პირდაპირი პროექცია ბრუნვისას "დაფანტულ" პიქსელებს დატოვებდა).
     */
    float inv_a, inv_b, inv_c, inv_d;

    /* დანიშნულების შემომსაზღვრელი მართკუთხედი (უკვე ეკრანზე მოჭრილი). */
    int   bb_x, bb_y, bb_w, bb_h;

    float alpha;

    /* TYPEWRITE: სტრიქონების გეომეტრია ტექსტურის სივრცეში. */
    float pad_y, line_height;
    int   line_count;
};

/*
 * ერთი ტექსტურის დაფენა კადრზე მასშტაბით და ბრუნვით.
 *
 * grid იფარება მხოლოდ *დანიშნულების* მართკუთხედზე (და არა მთელ ეკრანზე) —
 * 1080x1920 კადრზე პატარა სათაურისთვის ეს 50-ჯერ ნაკლები thread-ია.
 *
 * ბლენდი (premultiplied source-over):
 *      dst = src * alpha + dst * (1 - src.a * alpha)
 * `alpha` არის ვიჯეტის fade — premultiplied ფერზე მისი პირდაპირ გამრავლება
 * მართებულია, რადგან ის ერთდროულად ამცირებს ფერსაც და საფარველსაც.
 */
__global__ void k_composite_texture(uchar4 *__restrict__ fb,
                                    const uchar4 *__restrict__ tex,
                                    const float *__restrict__ cutoff_x,
                                    CompositeParams p)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    int j = blockIdx.y * blockDim.y + threadIdx.y;

    if (i >= p.bb_w || j >= p.bb_h) {
        return;
    }

    int gx = p.bb_x + i;
    int gy = p.bb_y + j;

    if (gx < 0 || gx >= p.fb_w || gy < 0 || gy >= p.fb_h) {
        return;
    }

    /* პიქსელის ცენტრი → ვექტორი ობიექტის ცენტრიდან კადრის სივრცეში. */
    float dx = (float)gx + 0.5f - p.cx;
    float dy = (float)gy + 0.5f - p.cy;

    /* უკუპროექცია ტექსტურის სივრცეში (ცენტრიდან). */
    float tx = p.inv_a * dx + p.inv_b * dy;
    float ty = p.inv_c * dx + p.inv_d * dy;

    /* ცენტრიდან ზედა-მარცხენა კუთხის ათვლაზე გადასვლა. */
    float u = tx + (float)p.tex_w * 0.5f;
    float v = ty + (float)p.tex_h * 0.5f;

    if (u < 0.0f || u >= (float)p.tex_w || v < 0.0f || v >= (float)p.tex_h) {
        return; /* ამ პიქსელს ტექსტურა არ ფარავს */
    }

    /*
     * TYPEWRITE — ზუსტად სიმბოლოს საზღვარზე მოჭრა.
     *
     * host-მა ყოველი სტრიქონისთვის უკვე გამოთვალა x-ზღვარი (რომელი სიმბოლოს
     * მარჯვენა კიდემდე ჩანს ტექსტი). აქ მხოლოდ ვადგენთ, რომელ სტრიქონში ვართ.
     */
    if (cutoff_x != NULL && p.line_count > 0) {
        int line = (int)floorf((v - p.pad_y) / p.line_height);
        line = min(max(line, 0), p.line_count - 1);
        if (u > cutoff_x[line]) {
            return;
        }
    }

    float4 src = sample_bilinear(tex, p.tex_w, p.tex_h, u, v);

    src.x *= p.alpha;
    src.y *= p.alpha;
    src.z *= p.alpha;
    src.w *= p.alpha;

    if (src.w <= 0.0f) {
        return; /* სრულიად გამჭვირვალე — read-modify-write-ს ვერიდებით */
    }

    size_t idx = (size_t)gy * p.fb_w + gx;
    uchar4 d   = fb[idx];

    const float inv     = 1.0f / 255.0f;
    float       inv_src = 1.0f - src.w;

    float r = src.x + (float)d.x * inv * inv_src;
    float g = src.y + (float)d.y * inv * inv_src;
    float b = src.z + (float)d.z * inv * inv_src;
    float a = src.w + (float)d.w * inv * inv_src;

    /* __saturatef ამაგრებს [0,1]-ში ერთი ინსტრუქციით (უფასოა). */
    fb[idx] = make_uchar4((unsigned char)(__saturatef(r) * 255.0f + 0.5f),
                          (unsigned char)(__saturatef(g) * 255.0f + 0.5f),
                          (unsigned char)(__saturatef(b) * 255.0f + 0.5f),
                          (unsigned char)(__saturatef(a) * 255.0f + 0.5f));
}

/* ------------------------------------------------------------------------- */
/* ტაიმლაინის შეფასება (host)                                                 */
/* ------------------------------------------------------------------------- */

static float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

/* შერბილების მრუდები anim.c-შია — იხ. easing_apply(). */

/* აქვს თუ არა ვიჯეტს მოცემული ტიპის მოვლენა (საწყისი მდგომარეობის დასადგენად). */
static bool widget_has_action(const Scene *scene, int widget_index, ActionType action)
{
    for (size_t i = 0; i < scene->event_count; i++) {
        if (scene->events[i].target_index == widget_index &&
            scene->events[i].action == action) {
            return true;
        }
    }
    return false;
}

/*
 * ავსებს `rt[]`-ს ყველა ვიჯეტისთვის დროის მომენტზე `time_ms`.
 *
 * ფუნქცია სუფთაა: შედეგი მხოლოდ ტაიმლაინსა და `time_ms`-ზეა დამოკიდებული,
 * და არა წინა კადრზე. ეს ნიშნავს, რომ ნებისმიერი კადრი შეიძლება ცალკე
 * დაირენდეროს (seek / preview / პარალელური რენდერი).
 *
 * მოვლენები გამოიყენება JSON-ის თანმიმდევრობით: MOVE-ის დელტები ჯამდება,
 * fade/scale/rotate კი ერთმანეთს გადააწერენ (ბოლო სიტყვა ბოლო მოვლენისაა).
 */
static void evaluate_scene(const EditorContext *ctx, const Scene *scene,
                           WidgetRuntime *rt, int local_ms)
{
    /* დრო სცენის შიგნით ლოკალურია — ტრეკებიც ამ ათვლას იყენებს. */
    float t_sec = (float)local_ms * 0.001f;

    for (size_t i = 0; i < scene->widget_count; i++) {
        const WidgetBase *b = ctx->widgets[scene->first_widget + i];

        /*
         * საბაზისო მნიშვნელობები. თუ თვისებას keyframe-ტრეკი აქვს, ის კარნახობს
         * აბსოლუტურ მნიშვნელობას; თუ არა — ობიექტის სტატიკური ველი გამოიყენება.
         * ტაიმლაინის მოვლენები ამის *შემდეგ* დაედება ზემოდან.
         */
        /* მიმაგრების წანაცვლება ორივე გზაზე ედება — სტატიკურსაც და ტრეკსაც. */
        rt[i].x        = (b->has_track_x ? track_sample(&b->tr_x, t_sec) : b->x)
                         - b->anchor_off_x;
        rt[i].y        = (b->has_track_y ? track_sample(&b->tr_y, t_sec) : b->y)
                         - b->anchor_off_y;
        rt[i].scale    = b->has_track_scale ? track_sample(&b->tr_scale, t_sec) : 1.0f;
        rt[i].visible  = true;

        /* ტრეკში კუთხე გრადუსებშია (როგორც JSON-ში), შიგნით — რადიანებში. */
        rt[i].rotation = b->has_track_rotation
                             ? track_sample(&b->tr_rotation, t_sec) * (float)(M_PI / 180.0)
                             : 0.0f;

        if (b->has_track_opacity) {
            rt[i].opacity = track_sample(&b->tr_opacity, t_sec);
        } else {
            /* თუ ვიჯეტს fade_in აქვს, ის დასაწყისში უხილავია; თუ არა — ჩანს t=0-დან. */
            rt[i].opacity = widget_has_action(scene, (int)i, ACTION_FADE_IN) ? 0.0f : 1.0f;
        }
        rt[i].reveal = widget_has_action(scene, (int)i, ACTION_TYPEWRITE) ? 0.0f : 1.0f;
    }

    for (size_t e = 0; e < scene->event_count; e++) {
        const TimelineEvent *ev = &scene->events[e];

        if (ev->target_index < 0 || (size_t)ev->target_index >= scene->widget_count) {
            continue; /* გაუხსნელი target ან უცნობი action */
        }

        int ev_ms = local_ms - ev->time_ms;
        if (ev_ms < 0) {
            continue; /* მოვლენა ჯერ არ დაწყებულა */
        }

        /* p — წრფივი პროგრესი 0..1; duration 0 ნიშნავს მყისიერ გადართვას. */
        float p = (ev->duration_ms > 0)
                      ? clampf((float)ev_ms / (float)ev->duration_ms, 0.0f, 1.0f)
                      : 1.0f;
        /* ყოველ მოვლენას თავისი მრუდი აქვს ("ease"), ნაგულისხმევი — smoothstep. */
        float eased = easing_apply(ev->ease, p);

        WidgetRuntime *r = &rt[ev->target_index];

        switch (ev->action) {
            case ACTION_FADE_IN:
                r->opacity = eased;
                break;
            case ACTION_FADE_OUT:
                r->opacity = 1.0f - eased;
                break;
            case ACTION_MOVE:
                r->x += ev->value_x * eased;
                r->y += ev->value_y * eased;
                break;
            case ACTION_TYPEWRITE:
                r->reveal = p; /* აკრეფა წრფივია — easing აქ არაბუნებრივია */
                break;
            case ACTION_SCALE:
                /* 1.0-დან სამიზნემდე ინტერპოლაცია. */
                r->scale = 1.0f + (ev->value - 1.0f) * eased;
                break;
            case ACTION_ROTATE:
                r->rotation = ev->value * eased * (float)(M_PI / 180.0);
                break;
            case ACTION_HIGHLIGHT:
            case ACTION_UNKNOWN:
            default:
                break;
        }
    }

    for (size_t i = 0; i < scene->widget_count; i++) {
        rt[i].opacity = clampf(rt[i].opacity, 0.0f, 1.0f);
        rt[i].reveal  = clampf(rt[i].reveal, 0.0f, 1.0f);
        if (rt[i].scale < 0.0f) {
            rt[i].scale = 0.0f;
        }
        /* 1/255-ზე ნაკლები საფარველი ვიზუალურად უხილავია → კერნელს ვერიდებით. */
        rt[i].visible = (rt[i].opacity > 0.002f) && (rt[i].reveal > 0.0f) &&
                        (rt[i].scale > 0.0001f);
    }
}

/*
 * აკრეფის ზღვრების გამოთვლა: `reveal` (0..1) → სტრიქონზე თითო x-კოორდინატი.
 *
 * ლოგიკა: ვითვლით რამდენი სიმბოლო ჩანს ჯამში, შემდეგ სტრიქონებს მიყოლებით
 * "ვახარჯავთ" ამ ბიუჯეტს. სრულად აკრეფილ სტრიქონს უსასრულო ზღვარი აქვს,
 * ჯერ მიუღწეველს — უარყოფითი (ანუ სრულიად დამალული).
 */
static void compute_reveal_cutoffs(const GlyphMetrics *g, float reveal, float *out)
{
    int budget = (int)floorf(reveal * (float)g->total_chars + 1e-4f);

    for (int l = 0; l < g->line_count; l++) {
        int chars_in_line = g->line_start[l + 1] - g->line_start[l] - 1;
        if (chars_in_line < 0) {
            chars_in_line = 0;
        }

        if (budget <= 0) {
            out[l] = -1.0e30f; /* ეს სტრიქონი ჯერ არ დაწყებულა */
        } else if (budget >= chars_in_line) {
            out[l] = 1.0e30f;  /* სტრიქონი სრულად ჩანს */
        } else {
            out[l] = g->char_x[g->line_start[l] + budget];
        }
        budget -= chars_in_line;
    }
}

/* ------------------------------------------------------------------------- */
/* FFmpeg-ის მილი                                                             */
/* ------------------------------------------------------------------------- */

/* shell-ის ციტირება საერთოა აუდიოს მიქსერთან — იხ. vr_shell_quote() audio.c-ში. */

/*
 * ხსნის ffmpeg-ს ქვეპროცესად და გვაძლევს მის stdin-ს.
 *
 * ჩვენ ვუგზავნით დაუმუშავებელ RGBA კადრებს; მთელი კოდირება NVENC-ის
 * აპარატურულ ბლოკზე ხდება, რომელიც CUDA-ს SM-ებისგან ფიზიკურად დამოუკიდებელია
 * — ანუ კოდირება რენდერს *არ* ანელებს.
 */
static FILE *open_ffmpeg_pipe(const EditorContext *ctx, const char *output_file)
{
    /* გარემოს ცვლადი JSON-ის "output" ბლოკზე მაღლა დგას — სწრაფი ექსპერიმენტისთვის. */
    const char *encoder = getenv("VIDEO_REDAC_ENCODER");
    if (encoder == NULL || encoder[0] == '\0') {
        encoder = (ctx->output.encoder != NULL) ? ctx->output.encoder : "h264_nvenc";
    }
    const char *preset = (ctx->output.preset != NULL) ? ctx->output.preset : "p5";

    char quoted_out[2048];
    char quoted_enc[128];
    char quoted_preset[64];
    if (!vr_shell_quote(output_file, quoted_out, sizeof quoted_out) ||
        !vr_shell_quote(encoder, quoted_enc, sizeof quoted_enc) ||
        !vr_shell_quote(preset, quoted_preset, sizeof quoted_preset)) {
        fprintf(stderr, "შეცდომა: გამომავალი ფაილის სახელი ძალიან გრძელია.\n");
        return NULL;
    }

    /* ბიტრეიტი მითითებულია → მუდმივი ბიტრეიტი; სხვა შემთხვევაში მუდმივი ხარისხი. */
    char rate[160];
    if (ctx->output.bitrate != NULL) {
        char quoted_br[64];
        if (!vr_shell_quote(ctx->output.bitrate, quoted_br, sizeof quoted_br)) {
            return NULL;
        }
        snprintf(rate, sizeof rate, "-b:v %s", quoted_br);
    } else {
        snprintf(rate, sizeof rate, "-rc vbr -cq %d -b:v 0", ctx->output.cq);
    }

    char cmd[4096];
    int  n = snprintf(cmd, sizeof cmd,
                      "ffmpeg -hide_banner -loglevel error -y "
                      "-f rawvideo -pixel_format nv12 -video_size %dx%d -framerate %d "
                      /*
                       * შემავალი ნაკადის ფერების ტეგირება *აუცილებელია*.
                       * მის გარეშე ffmpeg ვერ ხვდება, რომ NV12 უკვე BT.709
                       * limited-range-შია, საკუთარ ვარაუდს იყენებს და ჩუმად
                       * რთავს swscale-ს: "YUV color matrix differs for YUV->YUV,
                       * using intermediate RGB". ეს ერთდროულად ამახინჯებდა ფერებს
                       * (15/255-მდე ცდომილება) და CPU-ს ტვირთავდა ყოველ კადრზე.
                       */
                      "-colorspace bt709 -color_primaries bt709 -color_trc bt709 "
                      "-color_range tv -i - "
                      /* nv12 NVENC-ის ნატიური ფორმატია → გარდაქმნა საერთოდ არ ხდება. */
                      "-c:v %s -preset %s -tune hq %s -pix_fmt nv12 "
                      "-colorspace bt709 -color_primaries bt709 -color_trc bt709 "
                      "-color_range tv -movflags +faststart %s",
                      ctx->config.width, ctx->config.height, ctx->config.fps,
                      quoted_enc, quoted_preset, rate, quoted_out);

    if (n < 0 || (size_t)n >= sizeof cmd) {
        fprintf(stderr, "შეცდომა: ffmpeg-ის ბრძანება ბუფერში ვერ ჩაეტია.\n");
        return NULL;
    }

    fprintf(stderr, "FFmpeg: %s\n", cmd);

    FILE *pipe = popen(cmd, "w");
    if (pipe == NULL) {
        fprintf(stderr, "შეცდომა: ffmpeg ვერ გაეშვა (PATH-ში ხომ არის?).\n");
    }
    return pipe;
}

/* ------------------------------------------------------------------------- */
/* რესურსების ინიციალიზაცია / განთავისუფლება                                  */
/* ------------------------------------------------------------------------- */

/* ერთი ტექსტურის ატვირთვა VRAM-ში (თუ ჯერ არ აუტვირთავთ). */
static bool upload_one_texture(Texture *t, RenderResources *res)
{
    if (t->pixels == NULL || t->width <= 0 || t->height <= 0 || t->d_pixels != NULL) {
        return true; /* ცარიელი ან უკვე ატვირთული */
    }

    size_t bytes = (size_t)t->width * (size_t)t->height * sizeof(uchar4);

    void *d_ptr = NULL;
    CUDA_TRY(cudaMalloc(&d_ptr, bytes));
    CUDA_TRY(cudaMemcpy(d_ptr, t->pixels, bytes, cudaMemcpyHostToDevice));

    t->d_pixels = d_ptr;
    res->texture_bytes += bytes;
    res->texture_count++;
    return true;
}

/* ატვირთავს ყველა (უკვე დარასტერიზებულ) ტექსტურას VRAM-ში — ერთხელ. */
static bool upload_textures(EditorContext *ctx, RenderResources *res)
{
    for (size_t i = 0; i < ctx->widget_count; i++) {
        WidgetBase *b = ctx->widgets[i];

        if (!upload_one_texture(&b->tex, res)) {
            return false;
        }

        /* კოდის ბლოკის ფირფიტა ცალკე შრეა → ცალკე ტექსტურა. */
        if (b->kind == WIDGET_CODE) {
            CodeWidget *cw = (CodeWidget *)b;
            if (!upload_one_texture(&cw->plate, res)) {
                return false;
            }
        }

        /*
         * აკრეფის ზღვრების ბუფერი: VR_PIPELINE_DEPTH ცალი ნაკრები ერთ ალოკაციაში.
         * თითო სლოტს თავისი ნახევარი აქვს, რომ კადრი i-ს ჩაწერამ კადრი i-1-ის
         * ჯერ კიდევ მიმდინარე კერნელს მონაცემები არ გამოსტაცოს.
         */
        GlyphMetrics *g = &b->glyphs;
        if (g->line_count > 0 && g->d_cutoff == NULL) {
            size_t bytes = (size_t)g->line_count * VR_PIPELINE_DEPTH * sizeof(float);
            void  *d_ptr = NULL;
            CUDA_TRY(cudaMalloc(&d_ptr, bytes));
            g->d_cutoff = d_ptr;
            res->texture_bytes += bytes;
        }
    }
    return true;
}

bool renderer_init(EditorContext *ctx)
{
    if (ctx == NULL) {
        return false;
    }
    if (ctx->gpu != NULL) {
        return true; /* იდემპოტენტური */
    }

    RenderResources *res = (RenderResources *)calloc(1, sizeof(RenderResources));
    if (res == NULL) {
        return false;
    }
    ctx->gpu = res;

    int device_count = 0;
    CUDA_TRY(cudaGetDeviceCount(&device_count));
    if (device_count == 0) {
        fprintf(stderr, "შეცდომა: CUDA-ს მხარდამჭერი GPU ვერ მოიძებნა.\n");
        return false;
    }
    CUDA_TRY(cudaSetDevice(0));

    cudaDeviceProp prop;
    CUDA_TRY(cudaGetDeviceProperties(&prop, 0));
    fprintf(stderr, "GPU: %s (sm_%d%d, %.1f GiB VRAM)\n", prop.name, prop.major, prop.minor,
            (double)prop.totalGlobalMem / (1024.0 * 1024.0 * 1024.0));

    res->width       = ctx->config.width;
    res->height      = ctx->config.height;
    res->frame_bytes = (size_t)res->width * (size_t)res->height * sizeof(uchar4);

    /* NV12: სრული Y სიბრტყე + ნახევრად დისკრეტიზებული UV → w*h*3/2. */
    res->nv12_bytes = (size_t)res->width * (size_t)res->height +
                      (size_t)((res->width + 1) / 2) * (size_t)((res->height + 1) / 2) * 2;

    for (int s = 0; s < VR_PIPELINE_DEPTH; s++) {
        FrameSlot *slot = &res->slot[s];

        CUDA_TRY(cudaMalloc((void **)&slot->d_frame, res->frame_bytes));
        CUDA_TRY(cudaMalloc((void **)&slot->d_nv12, res->nv12_bytes));
        /* ping-pong ბუფერი მხოლოდ მაშინ, თუ ეფექტები საერთოდ არსებობს. */
        bool any_fx = ctx->effect_count > 0;
        for (size_t si = 0; si < ctx->scene_count && !any_fx; si++) {
            any_fx = ctx->scenes[si].effect_count > 0;
        }
        if (any_fx) {
            CUDA_TRY(cudaMalloc((void **)&slot->d_fx, res->frame_bytes));
        }
        /* სცენების ბუფერები მხოლოდ მაშინ, როცა გადასვლა საერთოდ შესაძლებელია. */
        if (ctx->scene_count > 1) {
            CUDA_TRY(cudaMalloc((void **)&slot->d_scene[0], res->frame_bytes));
            CUDA_TRY(cudaMalloc((void **)&slot->d_scene[1], res->frame_bytes));
        }

        /*
         * Pinned (page-locked) host მეხსიერება.
         * ჩვეულებრივი malloc-ის ბუფერისთვის დრაივერი ჯერ შიდა pinned ბუფერში
         * აკოპირებდა და მერე DMA-ს უშვებდა — ორმაგი სამუშაო. აქ DMA პირდაპირ
         * ჩვენს ბუფერში წერს, რაც D2H გადაცემას ~2-ჯერ აჩქარებს და, რაც მთავარია,
         * ნამდვილად ასინქრონულს ხდის.
         */
        CUDA_TRY(cudaHostAlloc((void **)&slot->h_frame, res->nv12_bytes, cudaHostAllocDefault));
        CUDA_TRY(cudaStreamCreate(&slot->stream));
        /* DisableTiming — ივენთი მხოლოდ სინქრონიზაციისთვისაა, ასე უფრო იაფია. */
        CUDA_TRY(cudaEventCreateWithFlags(&slot->done, cudaEventDisableTiming));
    }

    if (!upload_textures(ctx, res)) {
        return false;
    }

    fprintf(stderr, "VRAM: %d კადრის ბუფერი %.1f MiB + %d ტექსტურა %.1f MiB\n",
            VR_PIPELINE_DEPTH,
            (double)(res->frame_bytes + res->nv12_bytes) * VR_PIPELINE_DEPTH / (1024.0 * 1024.0),
            res->texture_count, (double)res->texture_bytes / (1024.0 * 1024.0));
    fprintf(stderr, "კადრი: %.2f MiB RGBA (VRAM) → %.2f MiB NV12 (მილში)\n",
            (double)res->frame_bytes / (1024.0 * 1024.0),
            (double)res->nv12_bytes / (1024.0 * 1024.0));
    return true;
}

void renderer_shutdown(EditorContext *ctx)
{
    if (ctx == NULL) {
        return;
    }

    /* ტექსტურების VRAM ასლები — მაშინაც კი, თუ RenderResources არ შექმნილა. */
    for (size_t i = 0; i < ctx->widget_count; i++) {
        WidgetBase *b = ctx->widgets[i];

        if (b->tex.d_pixels != NULL) {
            cudaFree(b->tex.d_pixels);
            b->tex.d_pixels = NULL;
        }
        if (b->glyphs.d_cutoff != NULL) {
            cudaFree(b->glyphs.d_cutoff);
            b->glyphs.d_cutoff = NULL;
        }
        if (b->kind == WIDGET_CODE) {
            CodeWidget *cw = (CodeWidget *)b;
            if (cw->plate.d_pixels != NULL) {
                cudaFree(cw->plate.d_pixels);
                cw->plate.d_pixels = NULL;
            }
        }
    }

    RenderResources *res = (RenderResources *)ctx->gpu;
    if (res == NULL) {
        return;
    }

    for (int s = 0; s < VR_PIPELINE_DEPTH; s++) {
        FrameSlot *slot = &res->slot[s];
        if (slot->d_frame != NULL) cudaFree(slot->d_frame);
        if (slot->d_nv12  != NULL) cudaFree(slot->d_nv12);
        if (slot->d_fx    != NULL) cudaFree(slot->d_fx);
        if (slot->d_scene[0] != NULL) cudaFree(slot->d_scene[0]);
        if (slot->d_scene[1] != NULL) cudaFree(slot->d_scene[1]);
        if (slot->h_frame != NULL) cudaFreeHost(slot->h_frame);
        if (slot->stream  != NULL) cudaStreamDestroy(slot->stream);
        if (slot->done    != NULL) cudaEventDestroy(slot->done);
    }

    free(res);
    ctx->gpu = NULL; /* → განმეორებითი გამოძახება უსაფრთხოა */
}

/* ------------------------------------------------------------------------- */
/* რენდერის მარყუჟი                                                           */
/* ------------------------------------------------------------------------- */

static double seconds_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) * 1e-9;
}

/*
 * ერთი შრის დაფენა: ითვლის გარდაქმნას და უშვებს კერნელს.
 *
 * `tex` შეიძლება იყოს გლიფების ტექსტურა ან ფირფიტა — გეომეტრია ორივესთვის
 * იდენტურია, განსხვავება მხოლოდ `cutoff`-შია (ფირფიტა არასოდეს იჭრება).
 */
static void composite_layer(const RenderResources *res, cudaStream_t stream, uchar4 *fb,
                            const Texture *tex, const WidgetBase *b, const WidgetRuntime *rt,
                            const float *d_cutoff)
{
    if (tex->d_pixels == NULL || tex->width <= 0 || tex->height <= 0) {
        return;
    }

    /* დანიშნულების ზომა: საბაზისო ზომა × ანიმაციის მასშტაბი. */
    float dst_w = b->base_w * rt->scale;
    float dst_h = b->base_h * rt->scale;
    if (dst_w < 0.5f || dst_h < 0.5f) {
        return;
    }

    /*
     * ბრუნვაც და მასშტაბიც ობიექტის *საბაზისო* ცენტრის ირგვლივ ხდება.
     *
     * ცენტრს განზრახ ვითვლით base_w/base_h-დან და არა dst_w/dst_h-დან: სხვა
     * შემთხვევაში ზრდისას ობიექტი ზედა-მარცხენა კუთხეზე "მიმაგრდებოდა" და
     * მარჯვნივ-ქვევით გაიწევდა — ცენტრირებული სათაური თვალსაჩინოდ აცდებოდა შუას.
     */
    float cx = rt->x + b->base_w * 0.5f;
    float cy = rt->y + b->base_h * 0.5f;

    float cs = cosf(rt->rotation);
    float sn = sinf(rt->rotation);

    /* ტექსტურის პიქსელები → დანიშნულების პიქსელები (ღერძების მასშტაბები). */
    float sx = dst_w / (float)tex->width;
    float sy = dst_h / (float)tex->height;

    CompositeParams p;
    p.fb_w  = res->width;
    p.fb_h  = res->height;
    p.tex_w = tex->width;
    p.tex_h = tex->height;
    p.cx    = cx;
    p.cy    = cy;

    /* t = S⁻¹ · R(-θ) · d  →  იხ. CompositeParams-ის კომენტარი. */
    p.inv_a =  cs / sx;
    p.inv_b =  sn / sx;
    p.inv_c = -sn / sy;
    p.inv_d =  cs / sy;

    /* მობრუნებული მართკუთხედის შემომსაზღვრელი ჩარჩო. */
    float abs_cs   = fabsf(cs);
    float abs_sn   = fabsf(sn);
    float half_bbw = (abs_cs * dst_w + abs_sn * dst_h) * 0.5f;
    float half_bbh = (abs_sn * dst_w + abs_cs * dst_h) * 0.5f;

    int x0 = (int)floorf(cx - half_bbw);
    int y0 = (int)floorf(cy - half_bbh);
    int x1 = (int)ceilf(cx + half_bbw) + 1;
    int y1 = (int)ceilf(cy + half_bbh) + 1;

    /* ეკრანზე მოჭრა — thread-ებს იმაზე არ ვხარჯავთ, რაც ისედაც არ დაიხატება. */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > res->width)  x1 = res->width;
    if (y1 > res->height) y1 = res->height;

    if (x1 <= x0 || y1 <= y0) {
        return; /* სრულიად ეკრანს გარეთაა */
    }

    p.bb_x  = x0;
    p.bb_y  = y0;
    p.bb_w  = x1 - x0;
    p.bb_h  = y1 - y0;
    p.alpha = rt->opacity;

    p.pad_y       = b->glyphs.pad_y;
    p.line_height = (b->glyphs.line_height > 0.0f) ? b->glyphs.line_height : 1.0f;
    p.line_count  = b->glyphs.line_count;

    const dim3 block(16, 16);
    dim3       grid((p.bb_w + block.x - 1) / block.x, (p.bb_h + block.y - 1) / block.y);

    k_composite_texture<<<grid, block, 0, stream>>>(fb, (const uchar4 *)tex->d_pixels,
                                                    d_cutoff, p);
    CUDA_CHECK_KERNEL();
}


/* ------------------------------------------------------------------------- */
/* ეფექტების სტეკის გაშვება                                                   */
/* ------------------------------------------------------------------------- */

/* Track-ების "გასინჯვა" და GPU-სთვის გასაგებ POD-ად აწყობა. */
static EffectGPU effect_sample(const Effect *fx, float t)
{
    EffectGPU g;
    memset(&g, 0, sizeof g);

    g.type = (int)fx->type;
    for (int i = 0; i < FXP_MAX; i++) {
        g.p[i] = track_sample(&fx->param[i], t);
    }

    const float inv = 1.0f / 255.0f;
    g.ca[0] = fx->color_a.r * inv; g.ca[1] = fx->color_a.g * inv;
    g.ca[2] = fx->color_a.b * inv; g.ca[3] = fx->color_a.a * inv;
    g.cb[0] = fx->color_b.r * inv; g.cb[1] = fx->color_b.g * inv;
    g.cb[2] = fx->color_b.b * inv; g.cb[3] = fx->color_b.a * inv;
    return g;
}

/*
 * ასრულებს მთელ სტეკს და აბრუნებს ბუფერს, რომელშიც საბოლოო კადრი დარჩა.
 *
 * ping-pong: ყოველი ეფექტი კითხულობს `src`-იდან და წერს `dst`-ში, შემდეგ ისინი
 * ადგილებს ცვლიან. ადგილზე (in-place) მუშაობა არ შეიძლება, რადგან მეზობლების
 * წამკითხავი ეფექტები (blur, glitch) უკვე შეცვლილ პიქსელებს წაიკითხავდნენ და
 * შედეგი მიმართულებაზე დამოკიდებული გახდებოდა.
 */
static uchar4 *apply_effect_list(const Effect *list, size_t count, RenderResources *res,
                                 int slot_idx, uchar4 *source, float t_sec, long long frame)
{
    FrameSlot *slot = &res->slot[slot_idx];
    uchar4    *src  = source;

    if (count == 0 || slot->d_fx == NULL) {
        return src;
    }

    uchar4 *dst = slot->d_fx;

    const dim3 block(16, 16);
    dim3       grid((res->width + block.x - 1) / block.x,
                    (res->height + block.y - 1) / block.y);

    /* მარცვლისა და glitch-ის მარცვალი კადრზეა მიბმული → ანიმაცია "ცოცხლობს",
     * მაგრამ ერთი და იგივე კადრი ყოველთვის ერთნაირად გამოიყურება. */
    unsigned int seed = (unsigned int)(frame * 2654435761ULL);

    for (size_t i = 0; i < count; i++) {
        const Effect *fx = &list[i];
        if (fx->type == FX_NONE) {
            continue;
        }

        EffectGPU g = effect_sample(fx, t_sec);

        switch (fx->type) {
            case FX_BLUR: {
                int radius = (int)lrintf(g.p[FXP_RADIUS]);
                if (radius < 1) {
                    continue; /* ნულოვანი რადიუსი — უბრალოდ გამოვტოვოთ */
                }
                if (radius > 128) {
                    radius = 128; /* ჭერი, რომ კერნელმა არ "დაიკიდოს" */
                }
                k_fx_blur<<<grid, block, 0, slot->stream>>>(dst, src, res->width,
                                                            res->height, radius, 1);
                CUDA_CHECK_KERNEL();
                uchar4 *tmp = src; src = dst; dst = tmp;

                k_fx_blur<<<grid, block, 0, slot->stream>>>(dst, src, res->width,
                                                            res->height, radius, 0);
                CUDA_CHECK_KERNEL();
                break;
            }
            case FX_PIXELATE:
                k_fx_pixelate<<<grid, block, 0, slot->stream>>>(
                    dst, src, res->width, res->height, (int)lrintf(g.p[FXP_SIZE]));
                CUDA_CHECK_KERNEL();
                break;

            case FX_RGB_SPLIT:
                k_fx_rgb_split<<<grid, block, 0, slot->stream>>>(
                    dst, src, res->width, res->height, g.p[FXP_AMOUNT], g.p[FXP_ANGLE]);
                CUDA_CHECK_KERNEL();
                break;

            case FX_GLITCH:
                k_fx_glitch<<<grid, block, 0, slot->stream>>>(
                    dst, src, res->width, res->height, g.p[FXP_AMOUNT], seed);
                CUDA_CHECK_KERNEL();
                break;

            default:
                k_fx_point<<<grid, block, 0, slot->stream>>>(dst, src, res->width,
                                                             res->height, g, seed);
                CUDA_CHECK_KERNEL();
                break;
        }

        uchar4 *tmp = src; src = dst; dst = tmp;
    }

    return src; /* swap-ების შემდეგ შედეგი აქაა */
}

/* ერთი სცენის დახატვა მითითებულ ბუფერში. */
static void render_scene_into(const EditorContext *ctx, RenderResources *res, int slot_idx,
                              const Scene *scene, const WidgetRuntime *rt, uchar4 *target)
{
    FrameSlot   *slot   = &res->slot[slot_idx];
    cudaStream_t stream = slot->stream;

    /* 1. ფონი — სცენას შეიძლება საკუთარი ჰქონდეს. */
    Color  bgc = scene->has_bg ? scene->bg_color : ctx->config.bg_color;
    uchar4 bg  = make_uchar4(bgc.r, bgc.g, bgc.b, bgc.a);

    const dim3 block(16, 16);
    dim3       grid((res->width + block.x - 1) / block.x,
                    (res->height + block.y - 1) / block.y);

    k_clear_background<<<grid, block, 0, stream>>>(target, res->width, res->height, bg);
    CUDA_CHECK_KERNEL();

    /* 2. ვიჯეტები — უკანა პლანიდან წინ (painter's algorithm). */
    for (size_t i = 0; i < scene->widget_count; i++) {
        const WidgetBase    *b = ctx->widgets[scene->first_widget + i];
        const WidgetRuntime *r = &rt[i];

        if (!r->visible) {
            continue;
        }

        /* 2a. ფირფიტა (მხოლოდ კოდის ბლოკს აქვს) — აკრეფა მას არ ეხება. */
        if (b->kind == WIDGET_CODE) {
            const CodeWidget *cw = (const CodeWidget *)b;
            composite_layer(res, stream, target, &cw->plate, b, r, NULL);
        }

        /* 2b. აკრეფის ზღვრები → VRAM (მხოლოდ თუ ტექსტი ნაწილობრივ ჩანს). */
        const float  *d_cut = NULL;
        GlyphMetrics *g     = (GlyphMetrics *)&b->glyphs;

        if (g->d_cutoff != NULL && g->h_cutoff != NULL && r->reveal < 0.9999f) {
            float *h_slice = g->h_cutoff + (size_t)slot_idx * g->line_count;
            float *d_slice = (float *)g->d_cutoff + (size_t)slot_idx * g->line_count;

            compute_reveal_cutoffs(g, r->reveal, h_slice);

            gpuErrchk(cudaMemcpyAsync(d_slice, h_slice,
                                      (size_t)g->line_count * sizeof(float),
                                      cudaMemcpyHostToDevice, stream));
            d_cut = d_slice;
        }

        /* 2c. თავად შრე. */
        composite_layer(res, stream, target, &b->tex, b, r, d_cut);
    }

}

/* ------------------------------------------------------------------------- */
/* გადასვლის პარამეტრები (host)                                               */
/* ------------------------------------------------------------------------- */

/* ერთი მხარის ინვერსიული მატრიცის აწყობა გადატანა/მასშტაბი/ბრუნვიდან. */
static void trans_side_set(TransSide *s, float opacity, float tx, float ty,
                           float scale, float rot_deg, int w, int h)
{
    if (scale < 0.001f) {
        scale = 0.001f;
    }
    float r  = rot_deg * (float)(M_PI / 180.0);
    float cs = cosf(r), sn = sinf(r);

    s->opacity = opacity;
    s->tx      = tx * (float)w;      /* გადატანა კადრის წილადებშია */
    s->ty      = ty * (float)h;
    s->ia      =  cs / scale;
    s->ib      =  sn / scale;
    s->ic      = -sn / scale;
    s->id      =  cs / scale;
    s->mask    = 0;
    s->m0 = s->m1 = s->m2 = s->m3 = 0.0f;
}

/*
 * preset-ის პარამეტრები პროგრესზე `p`.
 *
 * `from` ჯერ იხატება, `to` — ზემოდან, ამიტომ ბევრი გადასვლა მხოლოდ `to`-ს
 * მოძრაობაა. სრული სია types.h-ის TransitionType-შია.
 */
static void transition_preset(TransitionType type, float p, int w, int h,
                              TransSide *from, TransSide *to)
{
    trans_side_set(from, 1.0f, 0, 0, 1.0f, 0, w, h);
    trans_side_set(to,   1.0f, 0, 0, 1.0f, 0, w, h);

    switch (type) {
        case TRANS_CROSSFADE:
            to->opacity = p;
            break;

        case TRANS_FADE:                       /* ფონში გავლით */
            from->opacity = 1.0f - clampf(p * 2.0f, 0.0f, 1.0f);
            to->opacity   = clampf(p * 2.0f - 1.0f, 0.0f, 1.0f);
            break;

        case TRANS_SLIDE_LEFT:  trans_side_set(to, 1.0f,  (1.0f - p), 0, 1, 0, w, h); break;
        case TRANS_SLIDE_RIGHT: trans_side_set(to, 1.0f, -(1.0f - p), 0, 1, 0, w, h); break;
        case TRANS_SLIDE_UP:    trans_side_set(to, 1.0f, 0,  (1.0f - p), 1, 0, w, h); break;
        case TRANS_SLIDE_DOWN:  trans_side_set(to, 1.0f, 0, -(1.0f - p), 1, 0, w, h); break;

        case TRANS_PUSH_LEFT:
            trans_side_set(from, 1.0f, -p, 0, 1, 0, w, h);
            trans_side_set(to,   1.0f, (1.0f - p), 0, 1, 0, w, h);
            break;
        case TRANS_PUSH_RIGHT:
            trans_side_set(from, 1.0f, p, 0, 1, 0, w, h);
            trans_side_set(to,   1.0f, -(1.0f - p), 0, 1, 0, w, h);
            break;
        case TRANS_PUSH_UP:
            trans_side_set(from, 1.0f, 0, -p, 1, 0, w, h);
            trans_side_set(to,   1.0f, 0, (1.0f - p), 1, 0, w, h);
            break;
        case TRANS_PUSH_DOWN:
            trans_side_set(from, 1.0f, 0, p, 1, 0, w, h);
            trans_side_set(to,   1.0f, 0, -(1.0f - p), 1, 0, w, h);
            break;

        case TRANS_ZOOM_IN:
            trans_side_set(to, p, 0, 0, 0.7f + 0.3f * p, 0, w, h);
            break;
        case TRANS_ZOOM_OUT:
            trans_side_set(from, 1.0f - p, 0, 0, 1.0f + 0.35f * p, 0, w, h);
            trans_side_set(to,   p, 0, 0, 1.0f, 0, w, h);
            break;

        case TRANS_SPIN:
            trans_side_set(to, p, 0, 0, 0.2f + 0.8f * p, 180.0f * (1.0f - p), w, h);
            break;

        case TRANS_WIPE_RIGHT:                 /* მარცხნიდან მარჯვნივ იხსნება */
            to->mask = 2; to->m0 = 0.0f; to->m1 = 0.0f; to->m2 = p; to->m3 = 1.0f;
            break;
        case TRANS_WIPE_LEFT:
            to->mask = 2; to->m0 = 1.0f - p; to->m1 = 0.0f; to->m2 = p; to->m3 = 1.0f;
            break;

        case TRANS_IRIS:
            /* რადიუსი 0.75-მდე — კუთხეების დასაფარად საკმარისი (√2/2 ≈ 0.71). */
            to->mask = 1; to->m0 = 0.5f; to->m1 = 0.5f; to->m2 = p * 0.78f;
            break;

        case TRANS_CUT:
        default:
            to->opacity = (p >= 0.5f) ? 1.0f : 0.0f;
            break;
    }
}

/* JSON-ის `from`/`to` ტრეკები preset-ს გადააწერს. */
static void transition_apply_inline(const Transition *tr, float p, int w, int h,
                                    TransSide *from, TransSide *to)
{
    if (tr->has_from) {
        trans_side_set(from, track_sample(&tr->from_opacity, p),
                       track_sample(&tr->from_x, p), track_sample(&tr->from_y, p),
                       track_sample(&tr->from_scale, p),
                       track_sample(&tr->from_rotate, p), w, h);
    }
    if (tr->has_to) {
        trans_side_set(to, track_sample(&tr->to_opacity, p),
                       track_sample(&tr->to_x, p), track_sample(&tr->to_y, p),
                       track_sample(&tr->to_scale, p),
                       track_sample(&tr->to_rotate, p), w, h);
    }

    /* მასკები preset-ის მასკასაც გადააწერს — JSON ბოლო სიტყვას ამბობს. */
    if (tr->from_mask_shape != 0) {
        from->mask = tr->from_mask_shape;
        from->m0 = track_sample(&tr->from_mask[0], p);
        from->m1 = track_sample(&tr->from_mask[1], p);
        from->m2 = track_sample(&tr->from_mask[2], p);
        from->m3 = track_sample(&tr->from_mask[3], p);
    }
    if (tr->to_mask_shape != 0) {
        to->mask = tr->to_mask_shape;
        to->m0 = track_sample(&tr->to_mask[0], p);
        to->m1 = track_sample(&tr->to_mask[1], p);
        to->m2 = track_sample(&tr->to_mask[2], p);
        to->m3 = track_sample(&tr->to_mask[3], p);
    }
}

/*
 * სცენის საკუთარი ეფექტების დადება *ადგილზე*.
 *
 * ping-pong-ის გამო შედეგი შეიძლება დამხმარე ბუფერში აღმოჩნდეს; მაშინ უკან
 * ვაბრუნებთ, რომ გამომძახებელს იმავე მაჩვენებელზე შეეძლოს დაყრდნობა. ეს
 * ერთი D2D კოპირებაა და მხოლოდ მაშინ, თუ სცენას ეფექტები აქვს.
 */
static void scene_effects(const EditorContext *ctx, RenderResources *res, int slot_idx,
                          const Scene *scene, uchar4 *buffer, float t_local, long long frame)
{
    (void)ctx;
    if (scene->effect_count == 0) {
        return;
    }

    uchar4 *out = apply_effect_list(scene->effects, scene->effect_count, res, slot_idx,
                                    buffer, t_local, frame);
    if (out != buffer) {
        gpuErrchk(cudaMemcpyAsync(buffer, out, res->frame_bytes,
                                  cudaMemcpyDeviceToDevice, res->slot[slot_idx].stream));
    }
}

/* ------------------------------------------------------------------------- */
/* ერთი კადრი: სცენების შერჩევა, გადასვლა, ეფექტები, NV12                     */
/* ------------------------------------------------------------------------- */

static void render_one_frame(EditorContext *ctx, RenderResources *res, int slot_idx,
                             long long frame, int time_ms)
{
    FrameSlot   *slot   = &res->slot[slot_idx];
    cudaStream_t stream = slot->stream;
    float        t_sec  = (float)time_ms * 0.001f;

    /*
     * --- რომელი სცენა (ან ორი) ჩანს ამ წამს? ---------------------------
     *
     * ვეძებთ *პირველ* სცენას, რომელიც ჯერ არ დასრულებულა. ეს არსებითია:
     * გადაფარვის ფანჯარაში ორივე მეზობელი აქტიურია, და "from" სწორედ ის
     * წინაა. "ბოლო დაწყებული სცენის" აღება აქ შემდეგ წყვილს აირჩევდა და
     * გადასვლა საერთოდ არ დაიხატებოდა.
     */
    size_t si = ctx->scene_count - 1;
    for (size_t i = 0; i < ctx->scene_count; i++) {
        const Scene *sc = &ctx->scenes[i];
        if (time_ms < sc->start_ms + sc->duration_ms) {
            si = i;
            break;
        }
    }

    const Scene *A = &ctx->scenes[si];
    const Scene *B = NULL;
    float        p = 0.0f;

    if (si + 1 < ctx->scene_count) {
        const Scene *next = &ctx->scenes[si + 1];
        int          tdur = (si < ctx->transition_count)
                                ? ctx->transitions[si].duration_ms : 0;

        if (time_ms >= next->start_ms) {
            if (tdur > 0) {
                B = next;
                p = clampf((float)(time_ms - next->start_ms) / (float)tdur, 0.0f, 1.0f);
            } else {
                A = next;      /* ჭრა — უბრალოდ შემდეგი სცენა */
                si = si + 1;
            }
        }
    }

    /* --- სცენების დახატვა ---------------------------------------------- */
    uchar4 *base = slot->d_frame;

    if (B != NULL && slot->d_scene[0] != NULL) {
        arena_reset(&ctx->frame_arena);

        WidgetRuntime *rtA = ARENA_NEW(&ctx->frame_arena, WidgetRuntime,
                                       A->widget_count ? A->widget_count : 1);
        WidgetRuntime *rtB = ARENA_NEW(&ctx->frame_arena, WidgetRuntime,
                                       B->widget_count ? B->widget_count : 1);
        if (rtA == NULL || rtB == NULL) {
            return;
        }

        evaluate_scene(ctx, A, rtA, time_ms - A->start_ms);
        evaluate_scene(ctx, B, rtB, time_ms - B->start_ms);

        render_scene_into(ctx, res, slot_idx, A, rtA, slot->d_scene[0]);
        scene_effects(ctx, res, slot_idx, A, slot->d_scene[0],
                      (float)(time_ms - A->start_ms) * 0.001f, frame);

        render_scene_into(ctx, res, slot_idx, B, rtB, slot->d_scene[1]);
        scene_effects(ctx, res, slot_idx, B, slot->d_scene[1],
                      (float)(time_ms - B->start_ms) * 0.001f, frame);

        TransParams tp;
        tp.w = res->width;
        tp.h = res->height;
        Color bgc = ctx->config.bg_color;
        tp.bg = make_uchar4(bgc.r, bgc.g, bgc.b, 255);

        const Transition *tr = &ctx->transitions[si];
        transition_preset(tr->type, p, res->width, res->height, &tp.from, &tp.to);
        transition_apply_inline(tr, p, res->width, res->height, &tp.from, &tp.to);

        const dim3 block(16, 16);
        dim3       grid((res->width + block.x - 1) / block.x,
                        (res->height + block.y - 1) / block.y);

        k_transition<<<grid, block, 0, stream>>>(base, slot->d_scene[0],
                                                 slot->d_scene[1], tp);
        CUDA_CHECK_KERNEL();
    } else {
        arena_reset(&ctx->frame_arena);
        WidgetRuntime *rt = ARENA_NEW(&ctx->frame_arena, WidgetRuntime,
                                      A->widget_count ? A->widget_count : 1);
        if (rt == NULL) {
            return;
        }
        evaluate_scene(ctx, A, rt, time_ms - A->start_ms);
        render_scene_into(ctx, res, slot_idx, A, rt, base);
        scene_effects(ctx, res, slot_idx, A, base,
                      (float)(time_ms - A->start_ms) * 0.001f, frame);
    }

    /* 3. ფილმის მასშტაბის ეფექტები მზა კადრზე. */
    uchar4 *final_frame = apply_effect_list(ctx->effects, ctx->effect_count, res, slot_idx,
                                            base, t_sec, frame);

    /* 4. RGBA → NV12 იმავე VRAM-ში: გადასაცემი მოცულობა 2.67-ჯერ მცირდება. */
    uint8_t *y_plane  = slot->d_nv12;
    uint8_t *uv_plane = slot->d_nv12 + (size_t)res->width * (size_t)res->height;

    const dim3 nv_block(16, 16);
    dim3       nv_grid((((res->width + 1) / 2) + nv_block.x - 1) / nv_block.x,
                       (((res->height + 1) / 2) + nv_block.y - 1) / nv_block.y);

    k_rgba_to_nv12<<<nv_grid, nv_block, 0, stream>>>(final_frame, y_plane, uv_plane,
                                                     res->width, res->height);
    CUDA_CHECK_KERNEL();

    /* 5. უკან host-ში + ივენთი, რომ CPU-მ იცოდეს როდის დასრულდა. */
    gpuErrchk(cudaMemcpyAsync(slot->h_frame, slot->d_nv12, res->nv12_bytes,
                              cudaMemcpyDeviceToHost, stream));
    gpuErrchk(cudaEventRecord(slot->done, stream));
}

bool render_video(EditorContext *ctx, const char *output_file)
{
    if (ctx == NULL) {
        return false;
    }
    if (output_file == NULL || output_file[0] == '\0') {
        output_file = "output.mp4";
    }

    if (!renderer_init(ctx)) {
        fprintf(stderr, "შეცდომა: renderer-ის ინიციალიზაცია ჩავარდა.\n");
        return false;
    }
    RenderResources *res = (RenderResources *)ctx->gpu;

    /*
     * თუ პროექტს ხმა აქვს, ვიდეო ჯერ დროებით *მუნჯ* ფაილში იწერება, შემდეგ
     * მეორე გავლა ურთავს ფონოგრამას. ვიდეო-ნაკადი მაშინ ასლით გადადის, ანუ
     * მეორედ არ კოდირდება და ხარისხს არ კარგავს.
     */
    char       *silent_path  = NULL;
    const char *video_target = output_file;

    if (ctx->audio_count > 0) {
        silent_path = audio_make_silent_path(output_file);
        if (silent_path == NULL) {
            fprintf(stderr, "შეცდომა: დროებითი ფაილის სახელი ვერ აიგო.\n");
            return false;
        }
        video_target = silent_path;
    }

    /* კადრების რაოდენობა — ზემოთ დამრგვალებით, რომ ბოლო ნაწილობრივი კადრიც შევიდეს. */
    long long total_frames =
        ((long long)ctx->config.duration_ms * ctx->config.fps + 999) / 1000;
    if (total_frames < 1) {
        total_frames = 1;
    }

    /* --- გადასახედი დიაპაზონი ------------------------------------------- */
    long long first = 0;
    long long last  = total_frames - 1;

    if (ctx->range_end_sec > ctx->range_start_sec) {
        first = (long long)floor(ctx->range_start_sec * ctx->config.fps);
        last  = (long long)ceil(ctx->range_end_sec * ctx->config.fps) - 1;

        if (first < 0) first = 0;
        if (last > total_frames - 1) last = total_frames - 1;
        if (last < first) last = first;
    }
    long long count = last - first + 1;

    /*
     * SIGPIPE-ის ჩახშობა: თუ ffmpeg მოულოდნელად მოკვდება, fwrite ჩვენს პროცესს
     * სიგნალით მოკლავდა და არც ერთი გასუფთავება (VRAM, pipe) აღარ შესრულდებოდა.
     * ჩახშობის შემდეგ იგივე სიტუაცია უბრალოდ fwrite-ის შეცდომად გვიბრუნდება.
     */
    void (*old_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);

    FILE *pipe = open_ffmpeg_pipe(ctx, video_target);
    if (pipe == NULL) {
        signal(SIGPIPE, old_sigpipe);
        free(silent_path);
        return false;
    }

    if (count == total_frames) {
        fprintf(stderr, "რენდერი: %lld კადრი @ %d fps → %s\n",
                count, ctx->config.fps, output_file);
    } else {
        fprintf(stderr, "რენდერი: კადრები %lld..%lld (%lld ცალი, %.2f–%.2f წმ) @ %d fps → %s\n",
                first, last, count,
                (double)first / ctx->config.fps, (double)(last + 1) / ctx->config.fps,
                ctx->config.fps, output_file);
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    bool ok = true;

    /*
     * მილი: ყოველ იტერაციაზე ჯერ *წინა* კადრს ვწერთ (რომლის GPU-სამუშაოც უკვე
     * დასრულებულია), მერე მიმდინარეს ვუშვებთ. ასე NVENC-ის კოდირება და
     * კომპოზიტინგი ერთდროულად მიდის.
     *
     * უსაფრთხოება: სლოტ s-ის ბუფერების გადაწერამდე ვრწმუნდებით, რომ კადრი
     * i-2 (იმავე სლოტში) დასრულებულია — ეს გარანტირებულია, რადგან მისი ივენთი
     * წინა იტერაციაზე დაველოდეთ.
     */
    for (long long frame = first; frame <= last && ok; frame++) {
        int slot_idx = (int)(frame % VR_PIPELINE_DEPTH);

        int time_ms = (int)((frame * 1000) / ctx->config.fps);

        /* --- 1. წინა კადრის ჩაწერა, სანამ GPU მიმდინარეზე მუშაობს --------- */
        if (frame > first) {
            int        prev = (int)((frame - 1) % VR_PIPELINE_DEPTH);
            FrameSlot *ps   = &res->slot[prev];

            gpuErrchk(cudaEventSynchronize(ps->done));

            size_t written = fwrite(ps->h_frame, 1, res->nv12_bytes, pipe);
            if (written != res->nv12_bytes) {
                fprintf(stderr, "\nშეცდომა: ffmpeg-ის მილში ჩაწერა ჩავარდა "
                                "(%zu / %zu ბაიტი) — ენკოდერი ალბათ დაიხურა.\n",
                        written, res->nv12_bytes);
                ok = false;
                break;
            }
        }

        /* --- 2. მიმდინარე კადრის გაშვება (ასინქრონული) -------------------- */
        render_one_frame(ctx, res, slot_idx, frame, time_ms);

        long long done = frame - first + 1;
        if ((done % 30) == 1 || frame == last) {
            fprintf(stderr, "\r  %lld/%lld კადრი (%.1f%%)", done, count,
                    100.0 * (double)done / (double)count);
            fflush(stderr);
        }
    }

    /* --- 5. ბოლო კადრის "გამორეცხვა" მილიდან ------------------------------ */
    if (ok && count > 0) {
        int        last_slot = (int)(last % VR_PIPELINE_DEPTH);
        FrameSlot *ls        = &res->slot[last_slot];

        gpuErrchk(cudaEventSynchronize(ls->done));

        size_t written = fwrite(ls->h_frame, 1, res->nv12_bytes, pipe);
        if (written != res->nv12_bytes) {
            fprintf(stderr, "\nშეცდომა: ბოლო კადრის ჩაწერა ჩავარდა.\n");
            ok = false;
        }
    }

    /* ავარიულად გამოსვლისას GPU შეიძლება ჯერ კიდევ მუშაობდეს ჩვენს ბუფერებზე —
     * ვაცდით, სანამ მათ გავათავისუფლებდეთ. */
    gpuErrchk(cudaDeviceSynchronize());

    fprintf(stderr, "\n");

    int status = pclose(pipe);
    signal(SIGPIPE, old_sigpipe);

    if (status != 0) {
        fprintf(stderr, "გაფრთხილება: ffmpeg დასრულდა კოდით %d.\n", status);
        ok = false;
    }

    double elapsed = seconds_since(&start);

    /* --- 6. ხმის მეორე გავლა (თუ საჭიროა) --------------------------------- */
    if (silent_path != NULL) {
        bool muxed = false;
        if (ok) {
            muxed = audio_mux(ctx, silent_path, output_file);
            ok    = muxed;
        }

        if (muxed) {
            remove(silent_path);
        } else {
            /*
             * მიქსი ჩავარდა — მუნჯ ფაილს *არ* ვშლით. რენდერი შეიძლება წუთებს
             * გრძელდებოდეს და მისი გადაგდება ხმის გამო არასწორი იქნებოდა;
             * მომხმარებელს შეუძლია ხმა ხელით მიაერთოს ან თავიდან სცადოს.
             */
            fprintf(stderr, "შენიშვნა: მუნჯი ვიდეო შენარჩუნებულია — %s\n", silent_path);
        }
        free(silent_path);
    }

    if (ok) {
        fprintf(stderr, "მზადაა: %s — %lld კადრი %.2f წამში (%.1f fps)\n",
                output_file, count, elapsed,
                (elapsed > 0.0) ? (double)count / elapsed : 0.0);
    }

    return ok;
}
