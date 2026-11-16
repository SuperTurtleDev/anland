//! Real GBM/EGL rendering without acquiring a VT or constructing a KMS device.
use smithay::backend::allocator::gbm::{GbmAllocator, GbmBufferFlags, GbmDevice};
use smithay::backend::drm::DrmDeviceFd;
use smithay::backend::egl::EGLDisplay;
use smithay::backend::renderer::gles::GlesRenderer;
use smithay::utils::DeviceFd;
use std::error::Error;
use std::fs::OpenOptions;
use std::os::fd::OwnedFd;
use std::path::Path;

/// Keep this owner alive until Flutter, client imports and capture contexts
/// have been shut down. Fields drop in order: renderer, display, GBM, device.
pub(crate) struct RenderDevice {
    pub renderer: GlesRenderer,
    pub capture_allocator: GbmAllocator<DrmDeviceFd>,
    _display: EGLDisplay,
    _gbm: GbmDevice<DrmDeviceFd>,
    fd: DrmDeviceFd,
}

impl RenderDevice {
    /// The caller selects a real render device. No card0 or software fallback is
    /// inferred from the absence of physical outputs. GBM accepts or rejects the
    /// selected driver; permission/loader errors propagate to startup.
    pub fn open(path: &Path) -> Result<Self, Box<dyn Error>> {
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(path)
            .map_err(|error| {
                format!(
                    "could not open Anland render device {}: {error}",
                    path.display()
                )
            })?;
        Self::from_fd(file.into())
    }

    pub fn from_fd(fd: OwnedFd) -> Result<Self, Box<dyn Error>> {
        let fd = DrmDeviceFd::new(DeviceFd::from(fd));
        let gbm = GbmDevice::new(fd.clone()).map_err(|error| {
            format!("GBM initialization failed for the open Anland render device: {error}")
        })?;
        // SAFETY: this owner retains GBM and its fd until the renderer and EGL
        // display have been destroyed. No scanout buffers or DRM master needed.
        let display = unsafe { EGLDisplay::new(gbm.clone()) }?;
        let context = crate::egl_context::create_render_context(&display)?;
        // SAFETY: the context is transferred to the renderer, which is its sole
        // compositor-thread owner; Flutter creates its own shared contexts.
        let renderer = unsafe { GlesRenderer::new(context) }?;
        Ok(Self {
            renderer,
            capture_allocator: GbmAllocator::new(gbm.clone(), GbmBufferFlags::RENDERING),
            _display: display,
            _gbm: gbm,
            fd,
        })
    }

    /// For native device capability queries (e.g. syncobj). This does not claim
    /// modesetting support, DRM master, or consumer presentation timing.
    pub fn fd(&self) -> DrmDeviceFd {
        self.fd.clone()
    }
}
