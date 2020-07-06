// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_GFX_ENGINE_ENGINE_H_
#define SRC_UI_SCENIC_LIB_GFX_ENGINE_ENGINE_H_

#include <lib/fit/function.h>
#include <lib/sys/cpp/component_context.h>

#include <memory>
#include <set>
#include <vector>

#include <fbl/ref_ptr.h>

#include "lib/inspect/cpp/inspect.h"
#include "src/ui/lib/escher/escher.h"
#include "src/ui/lib/escher/flib/release_fence_signaller.h"
#include "src/ui/lib/escher/geometry/types.h"
#include "src/ui/lib/escher/renderer/batch_gpu_uploader.h"
#include "src/ui/lib/escher/resources/resource_recycler.h"
#include "src/ui/lib/escher/vk/image_factory.h"
#include "src/ui/scenic/lib/display/display_manager.h"
#include "src/ui/scenic/lib/gfx/engine/annotation_manager.h"
#include "src/ui/scenic/lib/gfx/engine/engine_renderer.h"
#include "src/ui/scenic/lib/gfx/engine/object_linker.h"
#include "src/ui/scenic/lib/gfx/engine/scene_graph.h"
#include "src/ui/scenic/lib/gfx/engine/session_context.h"
#include "src/ui/scenic/lib/gfx/engine/session_manager.h"
#include "src/ui/scenic/lib/gfx/resources/nodes/scene.h"
#include "src/ui/scenic/lib/gfx/sysmem.h"
#include "src/ui/scenic/lib/scenic/event_reporter.h"
#include "src/ui/scenic/lib/scheduling/delegating_frame_scheduler.h"
#include "src/ui/scenic/lib/scheduling/frame_scheduler.h"
#include "src/ui/scenic/lib/scheduling/id.h"

namespace scenic_impl {
namespace gfx {

class Compositor;
class Engine;
class View;
class ViewHolder;

using ViewLinker = ObjectLinker<ViewHolder*, View*>;
using PresentationInfo = fuchsia::images::PresentationInfo;
using OnPresentedCallback = fit::function<void(PresentationInfo)>;

// Manages the interactions between the scene graph, renderers, and displays,
// producing output when prompted through the scheduling::FrameRenderer
// interface.
//
// Engine synchronizes with other gfx components, including Screenshotter and
// Snapshotter, using ChainedSemaphoreGenerator.
//
// Each component waits on a semaphore it fetches from Escher's
// ChainedSemaphoreGenerator, which was generated by previous component before,
// and then signals a new semaphore ChainedSemaphoreGenerator generates for
// future components.
//
// (1) Synchronization between Engine and Screenshotter:
//
//    /-------- Engine --------\         /--- Screenshotter --\      /- Engine-\
//  GpuUploader         Renderer        Renderer + DownloadImage (*) GpuUploader
//     [TX]    --(i)-> [GRAPHICS] --(s)->     [GRAPHICS]       --(s)->  [TX]
//
// Note: [TX] means transfer queue and [GRAPHICS] means graphics queue.
//       --(i)-> means internal semaphore defined in EngineRenderer,
//       --(s)-> is the semaphore we generate (and fetch later) from
//               ChainedSemaphoreGenerator.
//
// The (*) semaphore ensures we won't upload HostImage until we finish taking
// the screenshot.
//
// (2) Synchronization between Engine and Snapshotter:
//
//    /-------- Engine --------\           /- Snapshotter -\      /- Engine-\
//   GpuUploader        Renderer    (*)     GpuDownloader  (**)   GpuUploader
//     [TX]    --(i)-> [GRAPHICS] --(s)->       [TX]     --(s)->     [TX]
//
// (*) semaphore enforces that we don't execute GpuDownloader (i.e. take
// snapshots) until we finish rendering; and (**) semaphore enforces we do not
// upload new HostImages until we finish downloading them all.
//
// (3) Synchronization between two frames:
//
//    /-------- Engine --------\            /-------- Engine --------\
//   GpuUploader        Renderer    (*)    GpuUploader        Renderer
//     [TX]    --(i)-> [GRAPHICS] --(s)->    [TX]    --(i)-> [GRAPHICS]
//
// (*) semaphore ensures GpuUploader will never upload to a HostImage if it
// hasn't finished rendering.
//
class Engine : public scheduling::FrameRenderer {
 public:
  Engine(sys::ComponentContext* app_context,
         const std::shared_ptr<scheduling::FrameScheduler>& frame_scheduler,
         escher::EscherWeakPtr escher, inspect::Node inspect_node);

  // Only used for testing.
  Engine(sys::ComponentContext* app_context,
         const std::shared_ptr<scheduling::FrameScheduler>& frame_scheduler,
         std::unique_ptr<escher::ReleaseFenceSignaller> release_fence_signaller,
         escher::EscherWeakPtr escher);

  ~Engine() override = default;

  escher::Escher* escher() const { return escher_.get(); }
  escher::EscherWeakPtr GetEscherWeakPtr() const { return escher_; }

  vk::Device vk_device() { return escher_ ? escher_->vulkan_context().device : vk::Device(); }

  EngineRenderer* renderer() { return engine_renderer_.get(); }

  // TODO(SCN-1151)
  // Instead of a set of Compositors, we should probably root at a set of
  // Displays. Or, we might not even need to store this set, and Displays (or
  // Compositors) would just be able to schedule a frame for themselves.
  SceneGraphWeakPtr scene_graph() { return scene_graph_.GetWeakPtr(); }

  ViewLinker* view_linker() { return &view_linker_; }

  AnnotationManager* annotation_manager() { return annotation_manager_.get(); }

  SessionContext session_context() {
    return SessionContext{vk_device(),
                          escher(),
                          escher_resource_recycler(),
                          escher_image_factory(),
                          release_fence_signaller(),
                          delegating_frame_scheduler_,
                          scene_graph(),
                          &view_linker_};
  }

  // Invoke Escher::Cleanup().  If more work remains afterward, post a delayed
  // task to try again; this is typically because cleanup couldn't finish due
  // to unfinished GPU work.
  void CleanupEscher();

  // Dumps the contents of all scene graphs.
  void DumpScenes(std::ostream& output,
                  std::unordered_set<GlobalId, GlobalId::Hash>* visited_resources) const;

  // |scheduling::FrameRenderer|
  //
  // Renders a new frame. Returns true if successful, false otherwise.
  scheduling::RenderFrameResult RenderFrame(fxl::WeakPtr<scheduling::FrameTimings> frame,
                                            zx::time presentation_time) override;

 private:
  // Initialize annotation session and annotation manager.
  void InitializeAnnotationManager();

  // Initialize all inspect::Nodes, so that the Engine state can be observed.
  void InitializeInspectObjects();

  // Takes care of cleanup between frames.
  void EndCurrentFrame(uint64_t frame_number);

  escher::ResourceRecycler* escher_resource_recycler() {
    return escher_ ? escher_->resource_recycler() : nullptr;
  }

  escher::ImageFactory* escher_image_factory() { return image_factory_.get(); }

  escher::ReleaseFenceSignaller* release_fence_signaller() {
    return release_fence_signaller_.get();
  }

  void InitializeShaderFs();

  // Returns true if layers contain content to be rendered.
  bool CheckForRenderableContent(const std::vector<HardwareLayerAssignment>& hlas);

  // Returns true if layers contain protected content.
  bool CheckForProtectedMemoryUse(const std::vector<HardwareLayerAssignment>& hlas);

  // Update and deliver metrics for all nodes which subscribe to metrics
  // events.
  void UpdateAndDeliverMetrics(zx::time presentation_time);

  // Update reported metrics for nodes which subscribe to metrics events.
  // If anything changed, append the node to |updated_nodes|.
  void UpdateMetrics(Node* node, const ::fuchsia::ui::gfx::Metrics& parent_metrics,
                     std::vector<Node*>* updated_nodes);

  const escher::EscherWeakPtr escher_;

  std::unique_ptr<EngineRenderer> engine_renderer_;

  ViewLinker view_linker_;

  std::unique_ptr<escher::ImageFactoryAdapter> image_factory_;
  std::unique_ptr<escher::ReleaseFenceSignaller> release_fence_signaller_;

  // TODO(SCN-1502): This is a temporary solution until we can remove frame_scheduler from
  // ResourceContext. Do not add any additional dependencies on this object/pointer.
  std::shared_ptr<scheduling::DelegatingFrameScheduler> delegating_frame_scheduler_;

  SceneGraph scene_graph_;

  bool escher_cleanup_scheduled_ = false;

  bool render_continuously_ = false;

  bool first_frame_ = true;

  bool last_frame_uses_protected_memory_ = false;

  std::unique_ptr<AnnotationManager> annotation_manager_;

  inspect::Node inspect_node_;
  inspect::LazyNode inspect_scene_dump_;

  fxl::WeakPtrFactory<Engine> weak_factory_;  // must be last

  FXL_DISALLOW_COPY_AND_ASSIGN(Engine);
};

}  // namespace gfx
}  // namespace scenic_impl

#endif  // SRC_UI_SCENIC_LIB_GFX_ENGINE_ENGINE_H_
