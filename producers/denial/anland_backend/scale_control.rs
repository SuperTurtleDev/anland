//! Compositor-side scaling only: the consumer still owns mode, rotation and pools.
use super::topology::{OUTPUT_NAME, OutputPlan};
use crate::options::{PersistedOutput, PreparedOutputConfig, prepare_output_config_persistence};
use crate::output_control::*;
use denial_core::topology::{OutputTransform, SCALE_BASE};
use std::path::PathBuf;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

pub(crate) struct ScaleControl {
    pub scale_120: u32,
    config: Option<PathBuf>,
    confirmation: Option<Confirmation>,
}

struct Confirmation {
    token: u64,
    deadline: Instant,
    deadline_unix_milliseconds: u64,
    rollback_scale_120: u32,
    persistence: Option<PreparedOutputConfig>,
    persistence_failure: Option<String>,
}

pub(crate) struct PreparedScale {
    pub plan: OutputPlan,
    timeout: Option<Duration>,
    persistence: Option<PreparedOutputConfig>,
}

impl ScaleControl {
    pub fn new(scale_120: u32, config: Option<PathBuf>) -> Result<Self, std::io::Error> {
        super::topology::validate_scale(scale_120)?;
        Ok(Self {
            scale_120,
            config,
            confirmation: None,
        })
    }

    pub fn state(&self, plan: Option<&OutputPlan>) -> OutputControlState {
        let available = plan.is_some();
        let capabilities = OutputControlCapabilities {
            apply: available,
            scale: available,
            persistent: self.config.is_some(),
            enable: false,
            position: false,
            mode: false,
            transform: false,
            scrolling_layout_axis: false,
            adaptive_sync: false,
            dpms: false,
            mirror: false,
            ten_bit: false,
        };
        let outputs = plan
            .map(|plan| {
                let spec = &plan.snapshot.outputs[0];
                let rect = spec.logical_rect();
                let mode = OutputControlMode {
                    width: plan.target.width,
                    height: plan.target.height,
                    refresh_millihz: plan.refresh_millihz,
                    preferred: true,
                };
                OutputControlOutput {
                    monitor_id: 1,
                    name: OUTPUT_NAME.into(),
                    description: "Anland consumer".into(),
                    connected: true,
                    enabled: true,
                    powered: true,
                    x: 0,
                    y: 0,
                    logical_width: rect.width.round().max(1.0) as u32,
                    logical_height: rect.height.round().max(1.0) as u32,
                    physical_width_mm: None,
                    physical_height_mm: None,
                    scale: f64::from(spec.scale_120) / f64::from(SCALE_BASE),
                    transform: OutputTransformName::Normal,
                    scrolling_layout_axis: Default::default(),
                    adaptive_sync_supported: false,
                    adaptive_sync: false,
                    current_mode: Some(mode),
                    modes: vec![mode],
                }
            })
            .into_iter()
            .collect();
        OutputControlState {
            capabilities,
            primary_output: plan.map(|_| OUTPUT_NAME.into()),
            outputs,
            pending_confirmation: self.confirmation.as_ref().map(|confirmation| {
                OutputControlConfirmation {
                    token: confirmation.token,
                    deadline_unix_milliseconds: confirmation.deadline_unix_milliseconds,
                }
            }),
        }
    }

    /// Validation and file staging happen before any live engine/Wayland mutation.
    pub fn prepare(
        &self,
        request: &ApplyOutputConfiguration,
        current: &OutputControlSnapshot,
        plan: &OutputPlan,
    ) -> Result<PreparedScale, OutputControlFailure> {
        if self.confirmation.is_some() {
            return Err(OutputControlFailure::new(
                "confirmation_pending",
                "keep or roll back the current scale before applying another",
            ));
        }
        let scale_120 = validate_request(request, current, self.config.is_some())?;
        let next = plan.with_scale(scale_120).map_err(|error| {
            OutputControlFailure::new("invalid_configuration", error.to_string())
        })?;
        let persistence = if request.persistent {
            let persisted = PersistedOutput {
                name: OUTPUT_NAME.into(),
                enabled: true,
                x: 0,
                y: 0,
                width: plan.target.width,
                height: plan.target.height,
                refresh_millihz: plan.refresh_millihz,
                scale_120,
                transform: OutputTransform::Normal,
                scrolling_layout_axis: Default::default(),
                adaptive_sync: false,
            };
            Some(
                prepare_output_config_persistence(
                    self.config.as_deref().unwrap(),
                    &[persisted],
                    Some(OUTPUT_NAME),
                )
                .map_err(|error| OutputControlFailure::new("persistence_failed", error))?,
            )
        } else {
            None
        };
        Ok(PreparedScale {
            plan: next,
            timeout: request
                .confirmation_timeout_milliseconds
                .map(Duration::from_millis),
            persistence,
        })
    }

    /// Called only after installing the staged geometry successfully. A timed
    /// preview never writes its file until Keep, including after a reconnect.
    pub fn applied(
        &mut self,
        prepared: PreparedScale,
        token: u64,
        now: Instant,
    ) -> Result<(), OutputControlFailure> {
        let previous = self.scale_120;
        self.scale_120 = prepared.plan.snapshot.outputs[0].scale_120;
        if let Some(timeout) = prepared.timeout {
            let wall_clock = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap_or_default();
            self.confirmation = Some(Confirmation {
                token,
                deadline: now + timeout,
                deadline_unix_milliseconds: (wall_clock + timeout)
                    .as_millis()
                    .min(u128::from(u64::MAX)) as u64,
                rollback_scale_120: previous,
                persistence: prepared.persistence,
                persistence_failure: None,
            });
        } else if let Some(persistence) = prepared.persistence {
            persistence
                .commit()
                .map_err(|error| OutputControlFailure::new("persistence_failed", error))?;
        }
        Ok(())
    }

    pub fn rollback_due(&self, now: Instant) -> Option<u32> {
        self.confirmation
            .as_ref()
            .filter(|confirmation| now >= confirmation.deadline)
            .map(|confirmation| confirmation.rollback_scale_120)
    }

    pub fn rollback_scale(&self, token: u64) -> Result<u32, OutputControlFailure> {
        let confirmation = self
            .confirmation
            .as_ref()
            .filter(|confirmation| confirmation.token == token)
            .ok_or_else(|| {
                OutputControlFailure::new("invalid_confirmation", "no matching scale preview")
            })?;
        Ok(confirmation.rollback_scale_120)
    }

    pub fn keep(&mut self, token: u64) -> Result<(), OutputControlFailure> {
        self.rollback_scale(token)?;
        let confirmation = self.confirmation.as_mut().unwrap();
        if let Some(persistence) = confirmation.persistence.take() {
            // PreparedOutputConfig::commit consumes its file staging. Retain
            // the token, rollback scale and original deadline on failure, but
            // never let a second Keep silently accept an unsaved preview.
            if let Err(error) = persistence.commit() {
                confirmation.persistence_failure = Some(error.clone());
                return Err(OutputControlFailure::new("persistence_failed", error));
            }
        }
        if let Some(error) = confirmation.persistence_failure.as_ref() {
            return Err(OutputControlFailure::new("persistence_failed", error.clone()));
        }
        self.confirmation = None;
        Ok(())
    }

    /// Also used when output disappears. The next imported pool gets the last
    /// kept scale; no preview is silently saved by hotplug or engine recovery.
    pub fn cancel_preview(&mut self) -> bool {
        if let Some(confirmation) = self.confirmation.take() {
            self.scale_120 = confirmation.rollback_scale_120;
            true
        } else {
            false
        }
    }
}

fn validate_request(
    request: &ApplyOutputConfiguration,
    current: &OutputControlSnapshot,
    persistence_available: bool,
) -> Result<u32, OutputControlFailure> {
    if request.serial != current.serial {
        return Err(OutputControlFailure::new(
            "stale_configuration",
            "output configuration changed; refresh and try again",
        ));
    }
    let [output] = request.outputs.as_slice() else {
        return Err(OutputControlFailure::new(
            "invalid_configuration",
            "Anland requires exactly one output",
        ));
    };
    let [live] = current.outputs.as_slice() else {
        return Err(OutputControlFailure::new(
            "output_unavailable",
            "Anland output is not active",
        ));
    };
    if output.name != live.name || output.name != OUTPUT_NAME {
        return Err(OutputControlFailure::new(
            "invalid_configuration",
            "unknown Anland output",
        ));
    }
    let mode = live.current_mode.ok_or_else(|| {
        OutputControlFailure::new("output_unavailable", "Anland has no current mode")
    })?;
    if output.enabled != live.enabled
        || output.powered != live.powered
        || output.x != live.x
        || output.y != live.y
        || output.mode.width != mode.width
        || output.mode.height != mode.height
        || output.mode.refresh_millihz != mode.refresh_millihz
        || output.transform != live.transform
        || output.adaptive_sync != live.adaptive_sync
        || output
            .scrolling_layout_axis
            .is_some_and(|axis| axis != live.scrolling_layout_axis)
        || request
            .primary_output
            .as_deref()
            .is_some_and(|name| name != OUTPUT_NAME)
    {
        return Err(OutputControlFailure::new(
            "unsupported",
            "Anland supports logical scale only; mode, position, rotation and power belong to the consumer",
        ));
    }
    if request.persistent && !persistence_available {
        return Err(OutputControlFailure::new(
            "unsupported",
            "no output configuration file was supplied",
        ));
    }
    if request
        .confirmation_timeout_milliseconds
        .is_some_and(|timeout| !(1_000..=60_000).contains(&timeout))
    {
        return Err(OutputControlFailure::new(
            "invalid_configuration",
            "confirmation timeout must be within [1000, 60000] milliseconds",
        ));
    }
    if !output.scale.is_finite() || !(0.25..=8.0).contains(&output.scale) {
        return Err(OutputControlFailure::new(
            "invalid_configuration",
            "scale must be finite and within [0.25, 8]",
        ));
    }
    Ok((output.scale * f64::from(SCALE_BASE)).round() as u32)
}
