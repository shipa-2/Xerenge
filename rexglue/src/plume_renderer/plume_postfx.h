/**
 * @file        plume_renderer/plume_postfx.h
 * @brief       Anti-aliasing (FXAA) and sharpening (CAS) of the finished frame.
 */
#pragma once

#include <cstdint>
#include <memory>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#include <plume_render_interface_types.h>

namespace rex::plume_renderer {

enum class AntiAliasing : uint32_t {
  kOff = 0,   // the console's look: no post-process
  kFxaa = 1,  // FXAA, over the frame at the size it is drawn at
};

// The settings, live: read once a frame, so a change made on any thread (the
// in-game menu) shows from the next frame on, with nothing to restart. They
// start from the environment - XERENGE_AA=fxaa|off (default off) and
// XERENGE_SHARPEN=0..1 (default 0, off) - which the launcher's `aa` and
// `sharpen` keys and run.sh's --aa= and --sharpen= set.
AntiAliasing GetAntiAliasing();
void SetAntiAliasing(AntiAliasing mode);
// 0 (off) to 1 (the strongest sharpening); values outside are clamped.
float GetSharpening();
void SetSharpening(float strength);

// The passes themselves, run by the swapchain between the scene target and
// the copy onto the window's image.
class PlumePostFx {
 public:
  // What a frame is made with: the live settings, taken once.
  struct Settings {
    bool fxaa = false;
    float sharpen = 0.0f;
    bool Any() const { return fxaa || sharpen > 0.0f; }
    static Settings Current();
  };

  PlumePostFx();
  ~PlumePostFx();
  PlumePostFx(const PlumePostFx&) = delete;
  PlumePostFx& operator=(const PlumePostFx&) = delete;

  void SetDevice(plume::RenderDevice* device) { device_ = device; }

  // Records the passes the settings ask for over `scene` (width x height) into
  // `list`, outside a render pass, and returns the texture holding the result
  // - `scene` itself, untouched, when there is nothing to do or the passes
  // could not be made. Its layout is the caller's to set for what comes next.
  // The targets and pipelines are made on first use.
  plume::RenderTexture* Record(plume::RenderCommandList* list, plume::RenderTexture* scene,
                               uint32_t width, uint32_t height, const Settings& settings);

  // Whether there are targets to give back (ReleaseTargets).
  bool HasTargets() const { return stage_a_.texture != nullptr || scene_.set != nullptr; }
  // Frees the targets, and the set that reads the scene target. Only with no
  // frame still on the GPU - and before the scene target is remade, which the
  // set points into.
  void ReleaseTargets();
  // Frees everything, the pipelines too.
  void Shutdown();

 private:
  // A texture the passes write, and the set that lets a pass read it.
  struct Stage {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
    std::unique_ptr<plume::RenderDescriptorSet> set;
  };

  bool EnsurePipelines();
  // A stage reading `scene`, or - as_target - a target of its own to write and read.
  bool MakeStage(Stage* stage, plume::RenderTexture* scene, uint32_t width, uint32_t height,
                 bool as_target);
  void Pass(plume::RenderCommandList* list, plume::RenderTexture* source,
            plume::RenderDescriptorSet* source_set, Stage* target, plume::RenderPipeline* pipeline,
            uint32_t width, uint32_t height, float sharpness);

  plume::RenderDevice* device_ = nullptr;
  bool pipelines_failed_ = false;
  std::unique_ptr<plume::RenderShader> vertex_shader_;
  std::unique_ptr<plume::RenderShader> fxaa_shader_;
  std::unique_ptr<plume::RenderShader> cas_shader_;
  std::unique_ptr<plume::RenderSampler> sampler_;
  plume::RenderDescriptorSetBuilder set_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderPipeline> fxaa_pipeline_;
  std::unique_ptr<plume::RenderPipeline> cas_pipeline_;

  // The scene target is only read: its set, here, and the view the set holds.
  Stage scene_;
  // FXAA's output, or CAS's when it runs alone; and CAS's after FXAA.
  Stage stage_a_;
  Stage stage_b_;
  // Passes have been recorded since the targets were made (for the log).
  bool announced_ = false;
};

}  // namespace rex::plume_renderer
