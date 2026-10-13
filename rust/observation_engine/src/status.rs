//! Per-dim status, replacing sentinel values (spec §6.2.2 principle 6; B6, B14).

use crate::clock::EventTime;

/// Why a dim has, or has not, a current value.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DimStatus {
    /// Not enough input yet: `have` of `need` (bars or trades of the dim's own clock).
    Warming { have: u32, need: u32 },
    /// A value computed from current input.
    Ready,
    /// The estimator was undefined on the current input (flat window, too few distinct values...):
    /// the value is missing, never a stand-in number.
    Invalid,
    /// Out of the estimator's valid range: the value is known only to be beyond the bound.
    Censored,
}

impl DimStatus {
    /// Wire code for `ObservationFrame::status` (a `u8` per dim).
    pub fn code(self) -> u8 {
        match self {
            DimStatus::Warming { .. } => 0,
            DimStatus::Ready => 1,
            DimStatus::Invalid => 2,
            DimStatus::Censored => 3,
        }
    }

    /// Is the value usable as an observation (missing dims are marginalized out by the HMM)?
    pub fn is_observed(self) -> bool {
        matches!(self, DimStatus::Ready)
    }
}

/// A dim's value with its status and the event time it was computed at.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct DimValue {
    pub value: f64,
    pub status: DimStatus,
    pub at: EventTime,
}

impl Default for DimValue {
    fn default() -> Self {
        Self { value: f64::NAN, status: DimStatus::Warming { have: 0, need: 1 }, at: EventTime(0) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_are_distinct_and_only_ready_is_observed() {
        let all = [DimStatus::Warming { have: 1, need: 2 }, DimStatus::Ready, DimStatus::Invalid, DimStatus::Censored];
        let codes: Vec<u8> = all.iter().map(|s| s.code()).collect();
        assert_eq!(codes, vec![0, 1, 2, 3]);
        assert_eq!(all.iter().filter(|s| s.is_observed()).count(), 1);
        assert!(DimValue::default().value.is_nan());
    }
}
