// Frame interpolation (F7, RX_FRAMEGEN=1): frames made between the game's, from its own two last frames, shown in
// between by a display thread, so a game at 60 fps is shown at 120 (RX_FRAMEGEN_MULT=3: 180) on a fast display.
// TOTK hands out no motion vectors, so the motion is estimated from the images (optical flow):
//   1. each frame is copied (it may be drawn over next frame) and reduced to quarter-size luma;
//   2. block matching: for every 16x16 pixels, the offset (within +-32 pixels) where the last frame's 8x8 quarter-size
//      patch matches the new frame's best (a small penalty for long vectors keeps still areas still), then a 3x3
//      median of those vectors;
//   3. each in-between frame at time t: every pixel takes the last frame back along t of its motion and the new frame
//      forward along 1 - t, blended by t.
// The display thread shows the in-between frames, then the new one, evenly over the game's frame time (its recent
// mean): what is shown runs about one frame behind the game. The HUD over moving scenery and things moving faster
// than the search reaches smear a little; loading screens (frames over 100 ms apart) are not interpolated.
// The threaded web build too: only its WebGPU thread may present to the canvas, and it may not sleep (the batches go
// through it), so there the made frames are shown by that thread's loop as they come due (framegen_web_tick). The
// single-threaded web build: not done (its one render thread is the replay).
#include "rx_internal.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

extern "C" void hos_host_thread_register_self(const char* name);

namespace rx {
namespace {

const char* kCopyWgsl = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var dst: texture_storage_2d<rgba8unorm, write>;
@compute @workgroup_size(8, 8) fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  let d = textureDimensions(dst);
  if (g.x >= d.x || g.y >= d.y) { return; }
  textureStore(dst, g.xy, vec4<f32>(textureLoad(src, g.xy, 0).rgb, 1.0));
}
)";

const char* kLumaWgsl = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var dst: texture_storage_2d<rgba16float, write>;
@compute @workgroup_size(8, 8) fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  let d = textureDimensions(dst);
  if (g.x >= d.x || g.y >= d.y) { return; }
  let s = textureDimensions(src) - vec2<u32>(1u);
  var sum = 0.0;
  for (var y = 0u; y < 4u; y++) {
    for (var x = 0u; x < 4u; x++) {
      sum += dot(textureLoad(src, min(g.xy * 4u + vec2<u32>(x, y), s), 0).rgb, vec3<f32>(0.299, 0.587, 0.114));
    }
  }
  textureStore(dst, g.xy, vec4<f32>(sum / 16.0, 0.0, 0.0, 1.0));
}
)";

const char* kDown2Wgsl = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var dst: texture_storage_2d<rgba16float, write>;
@compute @workgroup_size(8, 8) fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  let d = textureDimensions(dst);
  if (g.x >= d.x || g.y >= d.y) { return; }
  let s = textureDimensions(src) - vec2<u32>(1u);
  let b = g.xy * 2u;
  let v = textureLoad(src, min(b, s), 0).r
        + textureLoad(src, min(b + vec2<u32>(1u, 0u), s), 0).r
        + textureLoad(src, min(b + vec2<u32>(0u, 1u), s), 0).r
        + textureLoad(src, min(b + vec2<u32>(1u, 1u), s), 0).r;
  textureStore(dst, g.xy, vec4<f32>(v * 0.25, 0.0, 0.0, 1.0));
}
)";

// Coarse optical flow at pyramid level L2 (1/16 res): 1 vector per 2x2 L2 texels, 4x4 patch, R=3 (7x7 = 49 candidates, +-48 full-res px).
// Uses workgroup shared memory tiling (26x26 = 676 floats) so lb is loaded cooperatively instead of per-candidate global fetches.
const char* kFlowCoarseWgsl = R"(
@group(0) @binding(0) var la: texture_2d<f32>;
@group(0) @binding(1) var lb: texture_2d<f32>;
@group(0) @binding(2) var flow: texture_storage_2d<rgba16float, write>;
const TILE_W: i32 = 26;
var<workgroup> tile_b: array<f32, 676>;
@compute @workgroup_size(8, 8) fn main(
  @builtin(global_invocation_id) g: vec3<u32>,
  @builtin(local_invocation_id) lid: vec3<u32>,
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(local_invocation_index) lidx: u32
) {
  let lm = vec2<i32>(textureDimensions(la)) - vec2<i32>(1);
  // Tile origin in lb: group covers 8*2=16 texels, padded by R=3 + patch_half=2 -> origin = wid*16 - 5
  let tile_org = vec2<i32>(wid.xy) * 16 - vec2<i32>(5);
  for (var idx = i32(lidx); idx < TILE_W * TILE_W; idx += 64) {
    let tx = idx % TILE_W;
    let ty = idx / TILE_W;
    tile_b[idx] = textureLoad(lb, clamp(tile_org + vec2<i32>(tx, ty), vec2<i32>(0), lm), 0).r;
  }
  workgroupBarrier();

  let fd = textureDimensions(flow);
  if (g.x >= fd.x || g.y >= fd.y) { return; }
  let c = vec2<i32>(g.xy) * 2 + vec2<i32>(1);
  var pa: array<f32, 16>;
  for (var i = 0; i < 16; i++) {
    pa[i] = textureLoad(la, clamp(c + vec2<i32>(i % 4 - 2, i / 4 - 2), vec2<i32>(0), lm), 0).r;
  }
  let base_local = vec2<i32>(lid.xy) * 2 + vec2<i32>(6); // c - tile_org = lid*2 + 6
  var best = 1e9;
  var bv = vec2<i32>(0);
  for (var dy = -3; dy <= 3; dy++) {
    for (var dx = -3; dx <= 3; dx++) {
      var sad = 0.0;
      let o = base_local + vec2<i32>(dx, dy);
      for (var py = -2; py < 2; py++) {
        let row = (o.y + py) * TILE_W + (o.x - 2);
        let pi = (py + 2) * 4;
        sad += abs(pa[pi]     - tile_b[row])
             + abs(pa[pi + 1] - tile_b[row + 1])
             + abs(pa[pi + 2] - tile_b[row + 2])
             + abs(pa[pi + 3] - tile_b[row + 3]);
      }
      let cost = sad + 0.02 * f32(abs(dx) + abs(dy));
      if (cost < best) { best = cost; bv = vec2<i32>(dx, dy); }
    }
  }
  textureStore(flow, g.xy, vec4<f32>(vec2<f32>(bv), best / 16.0, 1.0));
}
)";

// Local refinement pass for L1 and L0: upscales prior level's vector x2 and searches R=2 (5x5 = 25 candidates).
// Uses 24x24 = 576 shared-memory tile for the local window around the coarse-predicted center when within bounds.
const char* kFlowRefineWgsl = R"(
@group(0) @binding(0) var la: texture_2d<f32>;
@group(0) @binding(1) var lb: texture_2d<f32>;
@group(0) @binding(2) var prev_flow: texture_2d<f32>;
@group(0) @binding(3) var flow: texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var<uniform> params: vec4<f32>; // x = scale_to_fullres (8.0 for L1, 4.0 for L0)
const TILE_W: i32 = 24;
var<workgroup> tile_b: array<f32, 576>;
@compute @workgroup_size(8, 8) fn main(
  @builtin(global_invocation_id) g: vec3<u32>,
  @builtin(local_invocation_id) lid: vec3<u32>,
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(local_invocation_index) lidx: u32
) {
  let lm = vec2<i32>(textureDimensions(la)) - vec2<i32>(1);
  let pfd = vec2<i32>(textureDimensions(prev_flow)) - vec2<i32>(1);
  // Shared group anchor prediction from the center of the workgroup's parent flow block
  let grp_seed = clamp(vec2<i32>(wid.xy * 4u + vec2<u32>(2u)), vec2<i32>(0), pfd);
  let grp_pred = vec2<i32>(round(textureLoad(prev_flow, grp_seed, 0).xy * 2.0));
  let tile_org = vec2<i32>(wid.xy) * 16 + grp_pred - vec2<i32>(4);
  for (var idx = i32(lidx); idx < TILE_W * TILE_W; idx += 64) {
    let tx = idx % TILE_W;
    let ty = idx / TILE_W;
    tile_b[idx] = textureLoad(lb, clamp(tile_org + vec2<i32>(tx, ty), vec2<i32>(0), lm), 0).r;
  }
  workgroupBarrier();

  let fd = textureDimensions(flow);
  if (g.x >= fd.x || g.y >= fd.y) { return; }
  let c = vec2<i32>(g.xy) * 2 + vec2<i32>(1);
  let pred = vec2<i32>(round(textureLoad(prev_flow, clamp(vec2<i32>(g.xy) / 2, vec2<i32>(0), pfd), 0).xy * 2.0));
  var pa: array<f32, 16>;
  for (var i = 0; i < 16; i++) {
    pa[i] = textureLoad(la, clamp(c + vec2<i32>(i % 4 - 2, i / 4 - 2), vec2<i32>(0), lm), 0).r;
  }
  var best = 1e9;
  var bv = pred;
  for (var dy = -2; dy <= 2; dy++) {
    for (var dx = -2; dx <= 2; dx++) {
      let off = pred + vec2<i32>(dx, dy);
      var sad = 0.0;
      let loc = c + off - tile_org;
      if (loc.x >= 2 && loc.y >= 2 && loc.x + 1 < TILE_W && loc.y + 1 < TILE_W) {
        for (var py = -2; py < 2; py++) {
          let row = (loc.y + py) * TILE_W + (loc.x - 2);
          let pi = (py + 2) * 4;
          sad += abs(pa[pi]     - tile_b[row])
               + abs(pa[pi + 1] - tile_b[row + 1])
               + abs(pa[pi + 2] - tile_b[row + 2])
               + abs(pa[pi + 3] - tile_b[row + 3]);
        }
      } else {
        for (var i = 0; i < 16; i++) {
          let q = clamp(c + off + vec2<i32>(i % 4 - 2, i / 4 - 2), vec2<i32>(0), lm);
          sad += abs(pa[i] - textureLoad(lb, q, 0).r);
        }
      }
      let cost = sad + 0.015 * f32(abs(off.x) + abs(off.y));
      if (cost < best) { best = cost; bv = off; }
    }
  }
  textureStore(flow, g.xy, vec4<f32>(vec2<f32>(bv) * params.x, best / 16.0, 1.0));
}
)";

// Confidence- and luma-weighted 3x3 smoothing + median filter (preserves sharp HUD & foreground edges)
const char* kSmoothWgsl = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var luma: texture_2d<f32>;
@group(0) @binding(2) var dst: texture_storage_2d<rgba16float, write>;
fn med9(v: array<f32, 9>) -> f32 {
  var a = v;
  for (var i = 0; i < 5; i++) {
    for (var j = i + 1; j < 9; j++) {
      if (a[j] < a[i]) { let t = a[i]; a[i] = a[j]; a[j] = t; }
    }
  }
  return a[4];
}
@compute @workgroup_size(8, 8) fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  let d = textureDimensions(dst);
  if (g.x >= d.x || g.y >= d.y) { return; }
  let m = vec2<i32>(d) - vec2<i32>(1);
  let lm = vec2<i32>(textureDimensions(luma)) - vec2<i32>(1);
  let lc = textureLoad(luma, clamp(vec2<i32>(g.xy) * 2, vec2<i32>(0), lm), 0).r;
  var xs: array<f32, 9>;
  var ys: array<f32, 9>;
  var wsum = 0.0;
  var vsum = vec2<f32>(0.0);
  var e = 0.0;
  for (var i = 0; i < 9; i++) {
    let o = vec2<i32>(i % 3 - 1, i / 3 - 1);
    let q = clamp(vec2<i32>(g.xy) + o, vec2<i32>(0), m);
    let f = textureLoad(src, q, 0);
    let lq = textureLoad(luma, clamp(q * 2, vec2<i32>(0), lm), 0).r;
    let w = exp(-18.0 * abs(lq - lc)) / (1.0 + 12.0 * f.z);
    vsum += f.xy * w;
    wsum += w;
    xs[i] = f.x;
    ys[i] = f.y;
    e = max(e, f.z);
  }
  let med = vec2<f32>(med9(xs), med9(ys));
  let bil = select(med, vsum / wsum, wsum > 1e-5);
  let blended = mix(med, bil, 0.5);
  textureStore(dst, g.xy, vec4<f32>(blended, e, 1.0));
}
)";

// Disocclusion-aware synthesis at 8x8 flow block density
const char* kSynthWgsl = R"(
@group(0) @binding(0) var fa: texture_2d<f32>;
@group(0) @binding(1) var fb: texture_2d<f32>;
@group(0) @binding(2) var flow: texture_2d<f32>;
@group(0) @binding(3) var samp: sampler;
@group(0) @binding(4) var dst: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(5) var<uniform> u: vec4<f32>;   // x = t
fn fl(q: vec2<i32>) -> vec3<f32> {
  return textureLoad(flow, clamp(q, vec2<i32>(0), vec2<i32>(textureDimensions(flow)) - vec2<i32>(1)), 0).xyz;
}
@compute @workgroup_size(8, 8) fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  let d = textureDimensions(dst);
  if (g.x >= d.x || g.y >= d.y) { return; }
  let t = u.x;
  let p = vec2<f32>(g.xy) + 0.5;
  // 8x8 pixel block grid
  let fp = p / 8.0 - 0.5;
  let i = vec2<i32>(floor(fp));
  let w = fp - floor(fp);
  let f00 = fl(i);
  let f10 = fl(i + vec2<i32>(1, 0));
  let f01 = fl(i + vec2<i32>(0, 1));
  let f11 = fl(i + vec2<i32>(1, 1));
  let f = mix(mix(f00, f10, w.x), mix(f01, f11, w.x), w.y);
  let v = f.xy;
  let size = vec2<f32>(d);
  let a = textureSampleLevel(fa, samp, (p - t * v) / size, 0.0).rgb;
  let b = textureSampleLevel(fb, samp, (p + (1.0 - t) * v) / size, 0.0).rgb;
  // When flow error is high (disocclusion or fast unmodeled change), bias toward the temporally closer frame
  let diff = dot(abs(a - b), vec3<f32>(0.299, 0.587, 0.114));
  let disocc = clamp((f.z - 0.04) * 6.0 + (diff - 0.15) * 2.5, 0.0, 1.0);
  let hard_t = select(0.0, 1.0, t >= 0.5);
  let alpha = mix(t, hard_t, disocc * 0.65);
  textureStore(dst, g.xy, vec4<f32>(mix(a, b, alpha), 1.0));
}
)";

constexpr int kMaxMult = 4;

struct Tex {
    WGPUTexture tex = nullptr;
    WGPUTextureView view = nullptr;
};
Tex make_tex(uint32_t w, uint32_t h, WGPUTextureFormat f, WGPUTextureUsage usage, const char* label) {
    WGPUTextureDescriptor d{};
    d.label = sv(label);
    d.usage = usage;
    d.dimension = WGPUTextureDimension_2D;
    d.size = {w, h, 1};
    d.format = f;
    d.mipLevelCount = 1;
    d.sampleCount = 1;
    Tex t;
    t.tex = wgpuDeviceCreateTexture(dev.device, &d);
    t.view = wgpuTextureCreateView(t.tex, nullptr);
    return t;
}
void drop_tex(Tex& t) {
    if (t.view) wgpuTextureViewRelease(t.view);
    if (t.tex) wgpuTextureRelease(t.tex);
    t = {};
}

/// What the display thread shows for one game frame: the made frames, then the game's (its own copy each)
struct OutSet {
    Tex frame[kMaxMult];
    int made = 0;   // (how many of frame[] are made frames; frame[made] is the game's)
    bool busy = false;
};

struct Job {
    OutSet* set;
    uint32_t sw, sh;
    int32_t ww, wh;
    int64_t at;
};

struct State {
    bool init = false;
    uint32_t w = 0, h = 0;
    uint32_t qw0 = 0, qh0 = 0, qw1 = 0, qh1 = 0, qw2 = 0, qh2 = 0;
    uint32_t fw2 = 0, fh2 = 0, fw1 = 0, fh1 = 0, fw0 = 0, fh0 = 0;
    WGPUComputePipeline copy = nullptr, luma = nullptr, down2 = nullptr;
    WGPUComputePipeline flow_coarse = nullptr, flow_refine = nullptr, smooth = nullptr, synth = nullptr;
    WGPUSampler sampler = nullptr;
    WGPUBuffer t_buf[kMaxMult] = {};
    WGPUBuffer scale_l1_buf = nullptr, scale_l0_buf = nullptr;
    Tex frames[2], lumas0[2], lumas1[2], lumas2[2];
    Tex flow2, flow1, flow0, flow_smooth;
    WGPUBindGroup luma_bg[2] = {}, down1_bg[2] = {}, down2_bg[2] = {};
    WGPUBindGroup flow2_bg[2] = {}, flow1_bg[2] = {}, flow0_bg[2] = {}, smooth_bg[2] = {};
    WGPUBindGroup synth_bg[2][2][kMaxMult] = {};
    int cur = 0;
    bool have_prev = false;
    int64_t last_at = 0;
    OutSet sets[2];
    int next_set = 0;
    // the display thread's
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> jobs;
    bool thread_started = false;
} fg;

void drop_bg(WGPUBindGroup& bg) {
    if (bg) wgpuBindGroupRelease(bg);
    bg = nullptr;
}

WGPUBindGroup make_bg(WGPUComputePipeline pipe, std::initializer_list<WGPUBindGroupEntry> entries) {
    std::vector<WGPUBindGroupEntry> be(entries);
    for (size_t i = 0; i < be.size(); i++) be[i].binding = (uint32_t)i;
    WGPUBindGroupLayout layout = wgpuComputePipelineGetBindGroupLayout(pipe, 0);
    WGPUBindGroupDescriptor bd{};
    bd.layout = layout;
    bd.entryCount = be.size();
    bd.entries = be.data();
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(dev.device, &bd);
    wgpuBindGroupLayoutRelease(layout);
    return bg;
}

WGPUBindGroupEntry tex_entry(WGPUTextureView v) {
    WGPUBindGroupEntry e{};
    e.textureView = v;
    return e;
}

WGPUBindGroupEntry samp_entry(WGPUSampler s) {
    WGPUBindGroupEntry e{};
    e.sampler = s;
    return e;
}

WGPUBindGroupEntry buf_entry(WGPUBuffer b) {
    WGPUBindGroupEntry e{};
    e.buffer = b;
    e.size = 16;
    return e;
}

WGPUComputePipeline make_compute(const char* wgsl, const char* label) {
    WGPUShaderSourceWGSL src{};
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = sv(wgsl);
    WGPUShaderModuleDescriptor md{};
    md.nextInChain = &src.chain;
    md.label = sv(label);
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(dev.device, &md);
    WGPUComputePipelineDescriptor cd{};
    cd.label = sv(label);
    cd.compute.module = mod;
    cd.compute.entryPoint = sv("main");
    WGPUComputePipeline p = wgpuDeviceCreateComputePipeline(dev.device, &cd);   // (layout: from the shader)
    wgpuShaderModuleRelease(mod);
    return p;
}

void init_pipelines() {
    if (fg.init) return;
    fg.init = true;
    fg.copy = make_compute(kCopyWgsl, "rx framegen copy");
    fg.luma = make_compute(kLumaWgsl, "rx framegen luma");
    fg.down2 = make_compute(kDown2Wgsl, "rx framegen down2");
    fg.flow_coarse = make_compute(kFlowCoarseWgsl, "rx framegen flow coarse");
    fg.flow_refine = make_compute(kFlowRefineWgsl, "rx framegen flow refine");
    fg.smooth = make_compute(kSmoothWgsl, "rx framegen smooth");
    fg.synth = make_compute(kSynthWgsl, "rx framegen synth");
    WGPUSamplerDescriptor sd{};
    sd.addressModeU = sd.addressModeV = sd.addressModeW = WGPUAddressMode_ClampToEdge;
    sd.magFilter = sd.minFilter = WGPUFilterMode_Linear;
    sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sd.lodMaxClamp = 1.0f;
    sd.maxAnisotropy = 1;
    fg.sampler = wgpuDeviceCreateSampler(dev.device, &sd);
    for (WGPUBuffer& b : fg.t_buf) {
        WGPUBufferDescriptor d{};
        d.size = 16;
        d.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        b = wgpuDeviceCreateBuffer(dev.device, &d);
    }
    auto make_scale_buf = [](float scale) {
        WGPUBufferDescriptor d{};
        d.size = 16;
        d.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        WGPUBuffer b = wgpuDeviceCreateBuffer(dev.device, &d);
        const float v[4] = {scale, 0, 0, 0};
        wgpuQueueWriteBuffer(dev.queue, b, 0, v, sizeof v);
        return b;
    };
    fg.scale_l1_buf = make_scale_buf(1.0f);
    fg.scale_l0_buf = make_scale_buf(4.0f);
}

void ensure_size(uint32_t w, uint32_t h) {
    if (fg.w == w && fg.h == h) return;
    {   // (the display thread is done with the old sets first)
        std::unique_lock lk(fg.mu);
        fg.cv.wait(lk, [] { return fg.jobs.empty() && !fg.sets[0].busy && !fg.sets[1].busy; });
    }
    for (int i = 0; i < 2; i++) {
        drop_bg(fg.luma_bg[i]);
        drop_bg(fg.down1_bg[i]);
        drop_bg(fg.down2_bg[i]);
        drop_bg(fg.flow2_bg[i]);
        drop_bg(fg.flow1_bg[i]);
        drop_bg(fg.flow0_bg[i]);
        drop_bg(fg.smooth_bg[i]);
        for (int s = 0; s < 2; s++)
            for (int k = 0; k < kMaxMult; k++) drop_bg(fg.synth_bg[i][s][k]);
        drop_tex(fg.frames[i]);
        drop_tex(fg.lumas0[i]);
        drop_tex(fg.lumas1[i]);
        drop_tex(fg.lumas2[i]);
        for (Tex& t : fg.sets[i].frame) drop_tex(t);
    }
    drop_tex(fg.flow2);
    drop_tex(fg.flow1);
    drop_tex(fg.flow0);
    drop_tex(fg.flow_smooth);

    fg.w = w, fg.h = h;
    fg.qw0 = (w + 3) / 4;
    fg.qh0 = (h + 3) / 4;
    fg.qw1 = (fg.qw0 + 1) / 2;
    fg.qh1 = (fg.qh0 + 1) / 2;
    fg.qw2 = (fg.qw1 + 1) / 2;
    fg.qh2 = (fg.qh1 + 1) / 2;
    fg.fw2 = (fg.qw2 + 1) / 2;
    fg.fh2 = (fg.qh2 + 1) / 2;
    fg.fw1 = (fg.qw1 + 1) / 2;
    fg.fh1 = (fg.qh1 + 1) / 2;
    fg.fw0 = (fg.qw0 + 1) / 2;
    fg.fh0 = (fg.qh0 + 1) / 2;

    const WGPUTextureUsage rw = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
    for (int i = 0; i < 2; i++) {
        fg.frames[i] = make_tex(w, h, WGPUTextureFormat_RGBA8Unorm, rw | WGPUTextureUsage_CopySrc, "rx framegen frame");
        fg.lumas0[i] = make_tex(fg.qw0, fg.qh0, WGPUTextureFormat_RGBA16Float, rw, "rx framegen luma0");
        fg.lumas1[i] = make_tex(fg.qw1, fg.qh1, WGPUTextureFormat_RGBA16Float, rw, "rx framegen luma1");
        fg.lumas2[i] = make_tex(fg.qw2, fg.qh2, WGPUTextureFormat_RGBA16Float, rw, "rx framegen luma2");
        for (Tex& t : fg.sets[i].frame)
            t = make_tex(w, h, WGPUTextureFormat_RGBA8Unorm, rw | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc,
                         "rx framegen shown");
    }
    fg.flow2 = make_tex(fg.fw2, fg.fh2, WGPUTextureFormat_RGBA16Float, rw, "rx framegen flow2");
    fg.flow1 = make_tex(fg.fw1, fg.fh1, WGPUTextureFormat_RGBA16Float, rw, "rx framegen flow1");
    fg.flow0 = make_tex(fg.fw0, fg.fh0, WGPUTextureFormat_RGBA16Float, rw, "rx framegen flow0");
    fg.flow_smooth = make_tex(fg.fw0, fg.fh0, WGPUTextureFormat_RGBA16Float, rw, "rx framegen flow smooth");

    for (int cur = 0; cur < 2; cur++) {
        const int prev = cur ^ 1;
        fg.luma_bg[cur] = make_bg(fg.luma, {tex_entry(fg.frames[cur].view), tex_entry(fg.lumas0[cur].view)});
        fg.down1_bg[cur] = make_bg(fg.down2, {tex_entry(fg.lumas0[cur].view), tex_entry(fg.lumas1[cur].view)});
        fg.down2_bg[cur] = make_bg(fg.down2, {tex_entry(fg.lumas1[cur].view), tex_entry(fg.lumas2[cur].view)});
        fg.flow2_bg[cur] = make_bg(
            fg.flow_coarse, {tex_entry(fg.lumas2[prev].view), tex_entry(fg.lumas2[cur].view), tex_entry(fg.flow2.view)});
        fg.flow1_bg[cur] = make_bg(fg.flow_refine,
                                   {tex_entry(fg.lumas1[prev].view), tex_entry(fg.lumas1[cur].view),
                                    tex_entry(fg.flow2.view), tex_entry(fg.flow1.view), buf_entry(fg.scale_l1_buf)});
        fg.flow0_bg[cur] = make_bg(fg.flow_refine,
                                   {tex_entry(fg.lumas0[prev].view), tex_entry(fg.lumas0[cur].view),
                                    tex_entry(fg.flow1.view), tex_entry(fg.flow0.view), buf_entry(fg.scale_l0_buf)});
        fg.smooth_bg[cur] = make_bg(
            fg.smooth, {tex_entry(fg.flow0.view), tex_entry(fg.lumas0[cur].view), tex_entry(fg.flow_smooth.view)});
        for (int s = 0; s < 2; s++) {
            for (int k = 1; k < kMaxMult; k++) {
                fg.synth_bg[cur][s][k] =
                    make_bg(fg.synth, {tex_entry(fg.frames[prev].view), tex_entry(fg.frames[cur].view),
                                       tex_entry(fg.flow_smooth.view), samp_entry(fg.sampler),
                                       tex_entry(fg.sets[s].frame[k - 1].view), buf_entry(fg.t_buf[k])});
            }
        }
    }
    fg.have_prev = false;
}

void pass_dispatch(WGPUComputePassEncoder p, WGPUComputePipeline pipe, WGPUBindGroup bg, uint32_t w, uint32_t h) {
    wgpuComputePassEncoderSetPipeline(p, pipe);
    wgpuComputePassEncoderSetBindGroup(p, 0, bg, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(p, (w + 7) / 8, (h + 7) / 8, 1);
}

/// RX_FRAMEGEN_DUMP=<seconds> (checking): that long after interpolation started, the last frame, the first made one and the new
/// one as fg_prev.png, fg_mid.png, fg_cur.png (the GPU waited for)
void dump_png(WGPUTexture tex, uint32_t w, uint32_t h, const char* path) {
    const uint32_t row = (w * 4 + 255) & ~255u;
    WGPUBufferDescriptor bd{};
    bd.size = (uint64_t)row * h;
    bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    WGPUBuffer buf = wgpuDeviceCreateBuffer(dev.device, &bd);
    WGPUCommandEncoder e = wgpuDeviceCreateCommandEncoder(dev.device, nullptr);
    WGPUTexelCopyTextureInfo src{};
    src.texture = tex;
    WGPUTexelCopyBufferInfo dst{};
    dst.buffer = buf;
    dst.layout.bytesPerRow = row;
    dst.layout.rowsPerImage = h;
    const WGPUExtent3D extent{w, h, 1};
    wgpuCommandEncoderCopyTextureToBuffer(e, &src, &dst, &extent);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(e, nullptr);
    wgpuQueueSubmit(dev.queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(e);
    struct Done { volatile bool done = false; WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error; } done;
    WGPUBufferMapCallbackInfo mc{};
    mc.mode = WGPUCallbackMode_AllowSpontaneous;
    mc.callback = [](WGPUMapAsyncStatus st, WGPUStringView, void* u, void*) {
        static_cast<Done*>(u)->status = st;
        static_cast<Done*>(u)->done = true;
    };
    mc.userdata1 = &done;
    wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, bd.size, mc);
    while (!done.done) wgpuDevicePoll(dev.device, true, nullptr);
    if (done.status == WGPUMapAsyncStatus_Success) {
        write_png(path, static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(buf, 0, bd.size)), w, h, row);
        wgpuBufferUnmap(buf);
        hos_log("rx: frame interpolation: wrote %s", path);
    }
    wgpuBufferRelease(buf);
}

int multiplier() {
    static const int m = [] {
        const char* e = getenv("RX_FRAMEGEN_MULT");
        return e ? std::clamp(atoi(e), 2, kMaxMult) : 2;
    }();
    return m;
}

/// The display thread: each set's made frames, then the game's, spread over the game's recent frame time
void display_main() {
    hos_host_thread_register_self("rx display");
    int64_t interval = 16666667, last_at = 0;
    struct { int on_time, late, skipped; int64_t since; } stat{};   // (logged every 5 s)
    for (;;) {
        Job j;
        {
            std::unique_lock lk(fg.mu);
            fg.cv.wait(lk, [] { return !fg.jobs.empty(); });
            j = fg.jobs.front();
            fg.jobs.pop_front();
        }
        if (last_at) {
            const int64_t d = j.at - last_at;
            if (d > 0 && d < 100000000) interval = (interval * 7 + std::clamp<int64_t>(d, 4000000, 50000000)) / 8;
        }
        last_at = j.at;
        if (!stat.since) stat.since = j.at;
        if (j.at - stat.since > 5000000000LL) {
            log_limited("framegen_pace", 40, "rx: frame interpolation: %d frames shown on time, %d late (over 2 ms), %d "
                        "left out (the game's next frame came first)", stat.on_time, stat.late, stat.skipped);
            stat = {0, 0, 0, j.at};
        }
        const int shown = j.set->made + 1;
        const int64_t step = interval / shown;
        for (int k = 0; k <= j.set->made; k++) {
            wgpuTextureViewAddRef(j.set->frame[k].view);
            surface_present(j.set->frame[k].view, j.sw, j.sh, j.ww, j.wh, false);
            if (k == j.set->made) break;
            // the next one at its time; if the game's next frame comes first, the rest are left out (shown at once,
            // two presents back to back: the display shows one of them)
            const int64_t due = j.at + step * (k + 1);
            std::unique_lock lk(fg.mu);
            if (fg.cv.wait_until(lk, std::chrono::steady_clock::now() +
                                         std::chrono::nanoseconds(std::max<int64_t>(0, due - hos_now_ns())),
                                 [] { return !fg.jobs.empty(); })) {
                stat.skipped += j.set->made - k;
                break;
            }
            (hos_now_ns() - due > 2000000 ? stat.late : stat.on_time)++;
        }
        std::lock_guard lk(fg.mu);
        j.set->busy = false;
        fg.cv.notify_all();
    }
}

#if RX_WEB
// ---- the web (the WebGPU thread): the set being shown, frame by frame as they come due ----
struct WebShow {
    bool on = false;
    Job j{};
    int k = 0;
    int64_t step = 0;
} ws;
int64_t web_interval = 16666667, web_last_at = 0;
struct { int on_time, late, skipped; int64_t since; } wstat;   // (logged every 5 s)
void web_present_k(int64_t due) {
    if (due) (hos_now_ns() - due > 2000000 ? wstat.late : wstat.on_time)++;
    const Tex& t = ws.j.set->frame[ws.k];
    wgpuTextureViewAddRef(t.view);
    surface_present(t.view, ws.j.sw, ws.j.sh, ws.j.ww, ws.j.wh, false);
}
void web_finish() {
    ws.on = false;
    ws.j.set->busy = false;
    framegen_web_due = 0;
}
/// the game's next frame is here: the last set's frames not shown yet are left out (shown now, they would go out
/// back to back with the next set's first, and the browser shows one of the two)
void web_flush() {
    if (!ws.on) return;
    wstat.skipped += ws.j.set->made - ws.k;
    web_finish();
}
void web_start(const Job& j) {
    if (web_last_at) {
        const int64_t d = j.at - web_last_at;
        if (d > 0 && d < 100000000) web_interval = (web_interval * 7 + std::clamp<int64_t>(d, 4000000, 50000000)) / 8;
    }
    web_last_at = j.at;
    if (!wstat.since) wstat.since = j.at;
    if (j.at - wstat.since > 5000000000LL) {
        log_limited("framegen_pace", 40, "rx: frame interpolation: %d frames shown on time, %d late (over 2 ms), %d "
                    "left out (the WebGPU thread busy when they were due)", wstat.on_time, wstat.late, wstat.skipped);
        wstat = {0, 0, 0, j.at};
    }
    ws.on = true;
    ws.j = j;
    ws.k = 0;
    ws.step = web_interval / (j.set->made + 1);
    web_present_k(0);
    if (!j.set->made) web_finish();
    else framegen_web_due = j.at + ws.step;
}
#endif
}  // namespace

#if RX_WEB
int64_t framegen_web_due;

int64_t framegen_web_tick() {
    static bool in_tick;   // (a present yields to the event loop)
    if (!ws.on || in_tick) return -1;
    const int64_t due = ws.j.at + ws.step * (ws.k + 1), now = hos_now_ns();
    if (now < due) return due - now;
    in_tick = true;
    ws.k++;
    web_present_k(due);
    in_tick = false;
    if (ws.k >= ws.j.set->made) {
        web_finish();
        return -1;
    }
    framegen_web_due = due + ws.step;
    return std::max<int64_t>(0, framegen_web_due - hos_now_ns());
}
#endif

bool framegen_frame(WGPUTextureView view, uint32_t sw, uint32_t sh, int32_t ww, int32_t wh) {
#if RX_WEB && !RX_WEB_MT
    (void)view, (void)sw, (void)sh, (void)ww, (void)wh;
    log_limited("framegen_web", 1, "rx: frame interpolation: the threaded web build only (index_mt.html)");
    return false;
#else
    if (!sw || !sh) return false;
#if RX_WEB
    web_flush();   // (the set still being shown: done now; this thread is the one that would free it)
#endif
    init_pipelines();
    ensure_size(sw, sh);
    if (!fg.thread_started) {
        fg.thread_started = true;
#if !RX_WEB
        std::thread(display_main).detach();
#endif
        hos_log("rx: frame interpolation on: %dx the game's frames (made from its last two, shown evenly)", multiplier());
    }

    const int64_t now = hos_now_ns();
    const bool gap = fg.last_at && now - fg.last_at > 100000000;   // (a loading hitch: nothing to blend)
    fg.last_at = now;
    // a set the display thread is not showing
    OutSet* set;
    int set_idx = 0;
    static int64_t waited, frames, since;
    const int64_t w0 = hos_now_ns();
    {
        std::unique_lock lk(fg.mu);
        fg.cv.wait(lk, [] { return !fg.sets[fg.next_set].busy; });
        set_idx = fg.next_set;
        set = &fg.sets[set_idx];
        set->busy = true;
        fg.next_set ^= 1;
    }
    waited += hos_now_ns() - w0;
    frames++;
    if (!since) since = w0;
#if !RX_WEB   // (the web's sets are free by now: web_flush)
    if (w0 - since > 5000000000LL) {   // (the submit thread's wait for a set the display thread is done with)
        log_limited("framegen_wait", 40, "rx: frame interpolation: %.2f ms a frame waiting for the display thread",
                    waited / 1e6 / frames);
        waited = frames = 0;
        since = w0;
    }
#endif
    const int cur = fg.cur, prev = cur ^ 1;
    const int m = multiplier();
    const bool blend = fg.have_prev && !gap;
    for (int k = 1; k < m && blend; k++) {
        const float t[4] = {(float)k / (float)m, 0, 0, 0};
        wgpuQueueWriteBuffer(dev.queue, fg.t_buf[k], 0, t, sizeof t);
    }
    WGPUCommandEncoder e = wgpuDeviceCreateCommandEncoder(dev.device, nullptr);
    auto run_pass = [&](WGPUComputePipeline pipe, WGPUBindGroup bg, uint32_t w, uint32_t h) {
        WGPUComputePassEncoder p = wgpuCommandEncoderBeginComputePass(e, nullptr);
        pass_dispatch(p, pipe, bg, w, h);
        wgpuComputePassEncoderEnd(p);
        wgpuComputePassEncoderRelease(p);
    };
    {
        WGPUBindGroup copy_bg = make_bg(fg.copy, {tex_entry(view), tex_entry(fg.frames[cur].view)});
        run_pass(fg.copy, copy_bg, sw, sh);
        wgpuBindGroupRelease(copy_bg);
    }
    run_pass(fg.luma, fg.luma_bg[cur], fg.qw0, fg.qh0);
    run_pass(fg.down2, fg.down1_bg[cur], fg.qw1, fg.qh1);
    run_pass(fg.down2, fg.down2_bg[cur], fg.qw2, fg.qh2);
    set->made = 0;
    if (blend) {
        run_pass(fg.flow_coarse, fg.flow2_bg[cur], fg.fw2, fg.fh2);
        run_pass(fg.flow_refine, fg.flow1_bg[cur], fg.fw1, fg.fh1);
        run_pass(fg.flow_refine, fg.flow0_bg[cur], fg.fw0, fg.fh0);
        run_pass(fg.smooth, fg.smooth_bg[cur], fg.fw0, fg.fh0);
        WGPUComputePassEncoder sp = wgpuCommandEncoderBeginComputePass(e, nullptr);
        for (int k = 1; k < m; k++) {
            pass_dispatch(sp, fg.synth, fg.synth_bg[cur][set_idx][k], sw, sh);
        }
        wgpuComputePassEncoderEnd(sp);
        wgpuComputePassEncoderRelease(sp);
        set->made = m - 1;
    }
    WGPUTexelCopyTextureInfo src{}, dst{};
    src.texture = fg.frames[cur].tex;
    dst.texture = set->frame[set->made].tex;
    const WGPUExtent3D extent{sw, sh, 1};
    wgpuCommandEncoderCopyTextureToTexture(e, &src, &dst, &extent);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(e, nullptr);
    wgpuQueueSubmit(dev.queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(e);
    wgpuTextureViewRelease(view);
    if (static const double dump_at = getenv("RX_FRAMEGEN_DUMP") ? atof(getenv("RX_FRAMEGEN_DUMP")) : 0; dump_at && blend) {
        static const int64_t t0 = now;
        static bool dumped;
        if (!dumped && now - t0 > (int64_t)(dump_at * 1e9)) {
            dumped = true;
            dump_png(fg.frames[prev].tex, sw, sh, "fg_prev.png");
            dump_png(set->frame[0].tex, sw, sh, "fg_mid.png");
            dump_png(fg.frames[cur].tex, sw, sh, "fg_cur.png");
        }
    }
    fg.have_prev = true;
    fg.cur = prev;
#if RX_WEB
    web_start({set, sw, sh, ww, wh, now});
#else
    {
        std::lock_guard lk(fg.mu);
        fg.jobs.push_back({set, sw, sh, ww, wh, now});
        fg.cv.notify_all();
    }
#endif
    return true;
#endif
}
}  // namespace rx
