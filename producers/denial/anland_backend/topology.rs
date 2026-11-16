//! Consumer geometry translated into Denial's native single-output topology.
use super::client::{Output, Target};
use super::submission::Pool;
use denial_core::topology::{
    AtlasPlan, LogicalPoint, OutputId, OutputSpec, OutputTransform, PixelSize, SCALE_BASE,
    TopologyChange, TopologyManager, TopologySnapshot,
};
use std::io;

pub(crate) const OUTPUT_ID: OutputId = OutputId(1);
pub(crate) const OUTPUT_NAME: &str = "Anland-1";

pub(crate) fn validate_scale(scale_120: u32) -> io::Result<()> {
    if !(SCALE_BASE / 4..=SCALE_BASE * 8).contains(&scale_120) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "scale must be within [0.25, 8]",
        ));
    }
    Ok(())
}

/// Staged geometry only. The owner publishes this after importing the targets
/// and retiring the previous pool; failure leaves the live topology unchanged.
#[derive(Clone)]
pub(crate) struct OutputPlan {
    pub topology: TopologyManager,
    pub snapshot: TopologySnapshot,
    pub atlas: AtlasPlan,
    pub pool: Pool,
    pub target: Target,
    pub refresh_millihz: u32,
}

impl OutputPlan {
    /// Refresh-only changes retain the imported native pool identity.
    pub fn set_refresh(&mut self, refresh: u32) -> io::Result<bool> {
        if refresh == 0 || refresh > i32::MAX as u32 {
            return Err(io::Error::other("invalid refresh rate"));
        }
        if self.refresh_millihz == refresh {
            return Ok(false);
        }
        let mut spec = self
            .snapshot
            .outputs
            .iter()
            .find(|s| s.id == OUTPUT_ID)
            .ok_or_else(|| io::Error::other("Anland output missing"))?
            .clone();
        spec.refresh_millihz = refresh;
        self.topology
            .apply([TopologyChange::Upsert(spec)])
            .map_err(io::Error::other)?;
        self.snapshot = self.topology.snapshot();
        self.atlas = AtlasPlan::for_snapshot(&self.snapshot)
            .ok_or_else(|| io::Error::other("invalid atlas"))?;
        self.refresh_millihz = refresh;
        Ok(true)
    }

    /// A logical projection change must never replace/rename the imported pool.
    pub fn with_scale(&self, scale_120: u32) -> io::Result<Self> {
        validate_scale(scale_120)?;
        let mut next = self.clone();
        let mut spec = next.snapshot.outputs[0].clone();
        spec.scale_120 = scale_120;
        next.topology
            .apply([TopologyChange::Upsert(spec)])
            .map_err(io::Error::other)?;
        next.snapshot = next.topology.snapshot();
        next.atlas = AtlasPlan::for_snapshot(&next.snapshot)
            .ok_or_else(|| io::Error::other("invalid scaled atlas"))?;
        if next.atlas.pixel_size != self.atlas.pixel_size {
            return Err(io::Error::other(
                "logical scale changed the physical Anland atlas",
            ));
        }
        Ok(next)
    }

    pub fn prepare(previous: &TopologyManager, output: Output, target: Target) -> io::Result<Self> {
        let scale_120 = previous
            .snapshot()
            .outputs
            .iter()
            .find(|spec| spec.id == OUTPUT_ID)
            .map_or(SCALE_BASE, |spec| spec.scale_120);
        Self::prepare_scaled(previous, output, target, scale_120)
    }

    pub fn prepare_scaled(
        previous: &TopologyManager,
        output: Output,
        target: Target,
        scale_120: u32,
    ) -> io::Result<Self> {
        validate_scale(scale_120)?;
        if target.generation == 0
            || !(1..=8).contains(&target.count)
            || target.index >= target.count
            || target.width == 0
            || target.height == 0
            || target.width > i32::MAX as u32
            || target.height > i32::MAX as u32
            || output.width != target.width
            || output.height != target.height
            || output.refresh_mhz == 0
            || output.refresh_mhz > i32::MAX as u32
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("invalid Anland output/target: output={output:?}, target={target:?}"),
            ));
        }
        if previous
            .snapshot()
            .outputs
            .iter()
            .any(|output| output.id != OUTPUT_ID || output.name != OUTPUT_NAME)
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "foreign output in Anland topology",
            ));
        }
        let mut topology = previous.clone();
        topology
            .apply([TopologyChange::Upsert(OutputSpec {
                id: OUTPUT_ID,
                name: OUTPUT_NAME.into(),
                position: LogicalPoint::new(0, 0),
                mode: PixelSize::new(output.width, output.height),
                scale_120,
                refresh_millihz: output.refresh_mhz,
                // Public geometry already describes the consumer's oriented image.
                // Scaling is a compositor logical projection, never a panel transform.
                transform: OutputTransform::Normal,
            })])
            .map_err(io::Error::other)?;
        let snapshot = topology.snapshot();
        let atlas = AtlasPlan::for_snapshot(&snapshot)
            .ok_or_else(|| io::Error::other("Anland topology has no atlas"))?;
        let plans = atlas
            .render_outputs(&snapshot)
            .ok_or_else(|| io::Error::other("Anland topology has no native render plan"))?;
        let [plan] = plans.as_slice() else {
            return Err(io::Error::other(
                "Anland requires one desktop render output",
            ));
        };
        let pool = Pool {
            generation: target.generation,
            configuration: plan.configuration_generation,
            output: plan.output_id,
            view: plan.render_view_id,
            size: plan.target_size,
            count: target.count as usize,
        };
        Ok(Self {
            topology,
            snapshot,
            atlas,
            pool,
            target,
            refresh_millihz: output.refresh_mhz,
        })
    }
}
