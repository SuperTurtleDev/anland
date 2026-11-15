//! Small Denial-specific hooks. No KMS completion is fabricated for an ACK.
use super::client::{Client, Target};
use super::display::NativeRenderer;
use super::submission::Pool;
use super::submission::{NativeOutput, Outcome};
use super::topology::OutputPlan;
use crate::flutter_launcher::FlutterLauncher;
use crate::flutter_runtime::{FlutterRuntime, ReadyOutputFrame};
use crate::flutter_runtime::{OutputRenderTargetPool, PoolSelection};
use crate::frame_request::{OutputFrameRequest, RenderDispatch};
use crate::wayland_frontend::WaylandFrontend;
use denial_core::topology::{AtlasPlan, OutputId, TopologyManager, TopologySnapshot};
use smithay::backend::allocator::dmabuf::{AsDmabuf, Dmabuf, DmabufFlags};
use smithay::backend::allocator::gbm::{GbmAllocator, GbmBuffer};
use smithay::backend::allocator::{Allocator, Buffer, Fourcc, Modifier};
use smithay::backend::drm::DrmDeviceFd;
use smithay::backend::egl::EGLContext;
use std::io;
use std::time::Instant;

struct SceneBuffer {
    dmabuf: Dmabuf,
    _allocation: GbmBuffer,
}

/// Consumer storage stays zero-copy. HDR alone gets native FP16 scene targets
/// and Denial's final BT.2020/PQ pass, retaining highlights from HDR clients.
pub(crate) struct ImportedPool {
    pub identity: Pool,
    pub hdr: bool,
    buffers: Vec<Dmabuf>,
    scenes: Vec<SceneBuffer>,
}

/// Called only after the public event queue and any old native pool have been
/// drained. Preparation does not publish output geometry or start the engine.
pub(crate) fn prepare_output(
    client: &mut Client,
    previous: &TopologyManager,
    scale_120: u32,
    allocator: &mut GbmAllocator<DrmDeviceFd>,
) -> io::Result<(OutputPlan, ImportedPool)> {
    if client.events_pending() != 0 {
        return Err(io::Error::from(io::ErrorKind::WouldBlock));
    }
    let target = client.writable()?.ok_or(io::ErrorKind::WouldBlock)?;
    let output = client.output()?;
    // Android may report its first rate after HELLO and the target-ready signal.
    // Retain this valid pool while the owner drains that metadata notification;
    // no fabricated frame clock and no destructive reconnect for pending data.
    if output.refresh_mhz == 0 {
        return Err(io::Error::new(
            io::ErrorKind::WouldBlock,
            "waiting for Anland consumer refresh rate",
        ));
    }
    let plan = OutputPlan::prepare_scaled(previous, output, target, scale_120)?;
    let imported = ImportedPool::import(client, target, plan.pool, allocator)?;
    let current = client.output()?;
    if client.events_pending() != 0
        || current.width != output.width
        || current.height != output.height
        || current.refresh_mhz != output.refresh_mhz
    {
        return Err(io::Error::from(io::ErrorKind::WouldBlock));
    }
    Ok((plan, imported))
}

impl ImportedPool {
    pub fn import(
        client: &mut Client,
        target: Target,
        identity: Pool,
        allocator: &mut GbmAllocator<DrmDeviceFd>,
    ) -> io::Result<Self> {
        if identity.generation != target.generation
            || identity.count != target.count as usize
            || !(1..=8).contains(&identity.count)
            || identity.configuration == 0
            || identity.size.width != target.width
            || identity.size.height != target.height
        {
            return Err(io::Error::other(
                "Anland pool identity differs from consumer targets",
            ));
        }
        let mut buffers = Vec::with_capacity(identity.count);
        let mut pool_format = None;
        for index in 0..target.count {
            let image = client.import(target.generation, index)?;
            if image.width != target.width || image.height != target.height {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "Anland image shape differs from pool",
                ));
            }
            let width = i32::try_from(image.width).map_err(io::Error::other)?;
            let height = i32::try_from(image.height).map_err(io::Error::other)?;
            // EGL import uses signed EGLint offsets and pitches.
            i32::try_from(image.stride).map_err(io::Error::other)?;
            i32::try_from(image.offset).map_err(io::Error::other)?;
            let format = Fourcc::try_from(image.fourcc)
                .map_err(|_| io::Error::other("unknown DRM fourcc"))?;
            if !matches!(
                format,
                Fourcc::Abgr8888 | Fourcc::Xbgr8888 | Fourcc::Argb8888 | Fourcc::Abgr2101010
            ) || pool_format.is_some_and(|first| first != format)
            {
                return Err(io::Error::other(
                    "unsupported or mixed Anland consumer formats",
                ));
            }
            pool_format = Some(format);
            let mut builder = Dmabuf::builder(
                (width, height),
                format,
                Modifier::from(image.modifier),
                DmabufFlags::empty(),
            );
            if !builder.add_plane(image.fd, image.offset, image.stride) {
                return Err(io::Error::other("could not add Anland dmabuf plane"));
            }
            buffers.push(
                builder
                    .build()
                    .ok_or_else(|| io::Error::other("invalid Anland dmabuf"))?,
            );
        }
        match client.writable()? {
            Some(current) if current.same_pool(target) => {}
            None => return Err(io::ErrorKind::WouldBlock.into()),
            Some(_) => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "Anland pool generation/shape changed during native import",
                ));
            }
        }
        let hdr = pool_format == Some(Fourcc::Abgr2101010);
        #[cfg(not(feature = "anland-runtime-v2"))]
        if hdr {
            return Err(io::Error::other(
                "this Denial version has no native HDR content pipeline",
            ));
        }
        let mut scenes = Vec::new();
        if hdr {
            for _ in 0..identity.count {
                // Use a native renderable, uncompressed FP16 scene. Do not
                // inherit the consumer's RGB10 layout or fall back to RGBA8.
                let allocation = allocator
                    .create_buffer(
                        target.width,
                        target.height,
                        Fourcc::Abgr16161616f,
                        &[Modifier::Linear],
                    )
                    .map_err(io::Error::other)?;
                if allocation.format().code != Fourcc::Abgr16161616f
                    || allocation.format().modifier != Modifier::Linear
                {
                    return Err(io::Error::other("HDR scene allocation is not linear FP16"));
                }
                scenes.push(SceneBuffer {
                    dmabuf: allocation.export().map_err(io::Error::other)?,
                    _allocation: allocation,
                });
            }
        }
        Ok(Self {
            identity,
            hdr,
            buffers,
            scenes,
        })
    }

    pub fn buffer(&self, index: usize) -> io::Result<&Dmabuf> {
        // Native capture reads the scene, before display primaries/PQ encoding.
        self.scenes
            .get(index)
            .map(|scene| &scene.dmabuf)
            .or_else(|| self.buffers.get(index))
            .ok_or_else(|| io::Error::other("capture source exceeds the imported Anland pool"))
    }

    pub fn descriptor(&self) -> OutputRenderTargetPool<'_> {
        OutputRenderTargetPool {
            output_id: self.identity.output,
            render_view_id: self.identity.view,
            configuration_generation: self.identity.configuration,
            size: self.identity.size,
            #[cfg(feature = "anland-runtime-v2")]
            color: self.hdr.then(|| super::hdr::output().output_color()),
            selection: PoolSelection::External {
                generation: self.identity.generation,
                selected: None,
            },
            transfers: self.buffers.iter().map(|_| None).collect(),
            dmabufs: self
                .buffers
                .iter()
                .enumerate()
                .map(|(index, buffer)| (buffer, self.scenes.get(index).map(|scene| &scene.dmabuf)))
                .collect(),
        }
    }

    pub fn begin_rebind_runtime(
        &self,
        runtime: &FlutterRuntime,
    ) -> Result<
        std::sync::mpsc::Receiver<denial_flutter_engine::RenderOperationResult>,
        Box<dyn std::error::Error>,
    > {
        runtime.begin_external_pool_replacement(
            crate::flutter_runtime::ExternalPoolDescriptor {
                generation: self.identity.generation,
                configuration: self.identity.configuration,
                output: self.identity.output,
                view: self.identity.view,
                size: self.identity.size,
                #[cfg(feature = "anland-runtime-v2")]
                color: self.hdr.then(|| super::hdr::output().output_color()),
            },
            self.buffers.clone(),
            #[cfg(feature = "anland-runtime-v2")]
            self.scenes
                .iter()
                .map(|scene| scene.dmabuf.clone())
                .collect(),
        )
    }

    /// Reuses Denial's shell factory, authentication, clipboard and packaged
    /// recovery path. The caller must provide a real initialized EGL context.
    /// No connector, CRTC, KMS framebuffer or initial scanout is fabricated.
    pub fn start_runtime(
        &self,
        launcher: &mut FlutterLauncher,
        context: &EGLContext,
        snapshot: &TopologySnapshot,
        atlas: &AtlasPlan,
        refresh_millihz: u32,
    ) -> Result<FlutterRuntime, Box<dyn std::error::Error>> {
        let plans = atlas
            .render_outputs(snapshot)
            .ok_or("Anland topology has no render plan")?;
        if plans.len() != 1
            || plans.iter().any(|plan| {
                plan.output_id != self.identity.output
                    || plan.render_view_id != self.identity.view
                    || plan.configuration_generation != self.identity.configuration
                    || plan.target_size != self.identity.size
            })
        {
            return Err("Anland render plan differs from the imported pool".into());
        }
        // Import every real FBO and validate RGB10/FP16 precision before
        // publishing a preferred HDR description or starting a float engine.
        let prepared =
            launcher.prepare_with_pools(context, [self.descriptor()], atlas.pixel_size)?;
        #[cfg(feature = "anland-runtime-v2")]
        super::hdr::activate(self.identity.output, self.hdr);
        let mut runtime = launcher.start_with_pools(
            context,
            [self.descriptor()],
            snapshot,
            atlas,
            refresh_millihz,
            Some(prepared),
        )?;
        runtime.enable_native_frame_clock();
        Ok(runtime)
    }
}

impl NativeOutput for FlutterRuntime {
    fn publish(&self, frame: &ReadyOutputFrame) -> io::Result<()> {
        self.publish_output(frame)
            .map_err(|error| io::Error::other(error.to_string()))
    }
    fn release(&self, output: OutputId, index: usize) -> io::Result<()> {
        self.release_output(output, index)
            .map_err(|error| io::Error::other(error.to_string()))
    }
}

impl NativeRenderer for FlutterRuntime {
    fn submission_allowed(&self, frame: &ReadyOutputFrame) -> bool {
        self.scene_recovery_frame_allowed(frame)
    }
    fn recovery_expired(&self, now: Instant) -> bool {
        self.scene_recovery_expired(now)
    }
    fn frame_accepted(&mut self, request: OutputFrameRequest) {
        self.scene_recovery_frame_accepted(request);
    }
    fn select(&self, pool: Pool, target: Target) -> bool {
        self.select_external_target(
            pool.output,
            target.generation,
            target.index as usize,
            target.count as usize,
        )
    }
    fn revoke(&self) {
        self.revoke_external_targets();
    }
    fn work_pending(&self, output: OutputId, now: Instant) -> bool {
        self.external_work_pending(output, now)
    }
    fn retirement_pending(&self, output: OutputId, now: Instant) -> bool {
        self.external_retirement_pending(output, now)
    }
    fn authorization_deadline(&self, output: OutputId) -> Option<Instant> {
        self.external_authorization_deadline(output)
    }
    fn render(
        &mut self,
        request: OutputFrameRequest,
        textures: &[i64],
    ) -> io::Result<RenderDispatch> {
        let flutter_output = self.with_frame_readiness(|pending, _| {
            pending.flutter_requested.then_some(request.tick.output)
        });
        self.render_authorized_output_status(&[request], textures.iter().copied(), flutter_output)
            .map_err(|error| io::Error::other(error.to_string()))
    }
    fn take_ready(&mut self, output: OutputId) -> Option<ReadyOutputFrame> {
        self.take_ready_frame(|candidate| candidate == output)
    }
}

pub(crate) fn deliver_feedback(
    frontend: &mut WaylandFrontend,
    outcome: &Outcome,
) -> io::Result<()> {
    let (frame, accepted) = match outcome {
        Outcome::Presented { frame, .. } => (frame, true),
        Outcome::Dropped { frame, .. } => (frame, false),
    };
    frontend.external_frame_completed(frame.pool.output, frame.request.tick.sequence, accepted);
    if let Outcome::Presented {
        frame, observed_at, ..
    } = outcome
    {
        // The current producer reports ACK without a clock-qualified timestamp.
        // Use the local observation instant and advertise no hardware flags.
        frontend
            .external_frame_presented(frame.pool.output, &frame.feedback, *observed_at)
            .map_err(|error| io::Error::other(error.to_string()))?;
        frontend.screencopy_output_presented(frame.pool.output, frame.capture_revision);
    }
    Ok(())
}
