//! init.rs -- Student-t HMM parameter initialization: k-means++ seeding (Arthur & Vassilvitskii
//! 2007) plus a hand-rolled PRNG to drive it.
//!
//! Ported from the ALGORITHM STRUCTURE of `tools/observation_vector/FeatureSaliencyEM.h`'s own
//! `KMeansPlusPlusSeed` (plan's own note, 2026-10-11: start from the existing, already-adopted
//! C++ port rather than re-deriving `student_t_hmm.py`'s `_kmeans_plusplus_seed_indices` cold) --
//! itself ported from that same Python function (`student_t_hmm.py`'s own docstring: "same
//! method, not a different one"). `train.rs`'s own `fit()` deliberately does not call this
//! module -- initialization is the caller's job, exactly as every golden-parity test in that file
//! already supplies an already-initialized model.
//!
//! **PRNG choice is a deliberately flagged, not-silently-resolved scope boundary.** The existing
//! C++ port uses `std::mt19937_64`; the Python original uses `np.random.default_rng` (PCG64).
//! Neither is realistically bit-exact-reproducible from Rust for the FULL pipeline anyway: the
//! layer above the raw bit generator (turning raw `u64`s into a uniform draw over `[0, n)` or
//! `[0.0, total)`) is itself implementation-defined even within the C++ standard alone (libstdc++
//! and libc++ use different `uniform_int_distribution`/`uniform_real_distribution` algorithms), so
//! chasing bit-for-bit cross-language parity at the distribution layer isn't a meaningful target
//! regardless of which raw generator this crate picks. What IS validated here: the raw bit
//! generator against its own canonical public reference implementation (not assumed from memory),
//! and the k-means++ algorithm's own structural correctness (distinct indices, correct degenerate-
//! case handling, deterministic reproducibility for a fixed seed).

/// SplitMix64 (Vigna, <https://prng.di.unimi.it/splitmix64.c>) -- a simple, well-specified,
/// single-function generator, hand-rolled per this crate's established "no dependency for a
/// well-defined, independently-testable algorithm" convention (`model.rs`'s `ln_gamma`,
/// `train.rs`'s `digamma`/`brent_root_find`). Not `std::mt19937_64` (used by the existing C++
/// k-means++ port this module starts from) -- see this module's own doc comment for why chasing
/// that specific choice wouldn't buy real cross-language parity anyway; SplitMix64 was chosen
/// instead for being small enough to hand-verify exactly (this module's own tests reproduce the
/// reference C implementation's published output bit-for-bit), not for matching any existing
/// reference's internal generator.
#[derive(Debug, Clone, Copy)]
pub struct SplitMix64 {
    state: u64,
}

impl SplitMix64 {
    pub fn new(seed: u64) -> Self {
        Self { state: seed }
    }

    /// Raw 64-bit output. Matches the reference C implementation's `next()` exactly (verified
    /// against a direct `gcc`-compiled run of that same reference code, this module's own tests).
    pub fn next_u64(&mut self) -> u64 {
        self.state = self.state.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.state;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    /// Uniform integer in `[0, bound)` via Lemire's method (2019, "Fast Random Integer Generation
    /// in an Interval") -- rejection only in the rare case the initial draw falls in the biased
    /// remainder region, not a naive (and low-index-biased) `% bound`.
    pub fn next_bounded(&mut self, bound: u64) -> u64 {
        if bound == 0 {
            return 0;
        }
        let mut m = u128::from(self.next_u64()) * u128::from(bound);
        let mut l = m as u64;
        if l < bound {
            let threshold = bound.wrapping_neg() % bound;
            while l < threshold {
                m = u128::from(self.next_u64()) * u128::from(bound);
                l = m as u64;
            }
        }
        (m >> 64) as u64
    }

    /// Uniform `f64` in `[0.0, 1.0)` -- standard top-53-bits-as-mantissa construction.
    pub fn next_f64(&mut self) -> f64 {
        (self.next_u64() >> 11) as f64 * (1.0 / (1u64 << 53) as f64)
    }
}

fn squared_distance<const D: usize>(a: &[f64; D], b: &[f64; D]) -> f64 {
    let mut s = 0.0;
    for j in 0..D {
        let d = a[j] - b[j];
        s += d * d;
    }
    s
}

/// k-means++ seeding: picks `k` distinct row indices out of `observations`, the first drawn
/// uniformly at random, each subsequent pick weighted by squared distance to the nearest
/// already-chosen center (cumulative-threshold sampling -- matches the existing C++ port's
/// `std::uniform_real_distribution(0, totalWeight)` + cumulative-sum-until-exceeded approach, not
/// Python's normalized-probability-array `rng.choice(n, p=probs)`; mathematically equivalent,
/// simpler without materializing a probability array). Returns fewer than `k` indices only if
/// `observations` itself has fewer than `k` rows.
///
/// **One confirmed, deliberately-not-silently-resolved divergence from the Python original**: in
/// the degenerate all-remaining-points-coincide case (`total_weight <= 0.0`), this -- matching the
/// existing C++ port exactly -- falls back to a plain uniform draw over ALL `n` indices, which
/// could re-select an already-chosen index. Python's own fallback explicitly excludes
/// already-chosen indices (`np.setdiff1d(..., chosen[:i])`). Not fixed here: the existing
/// institutional C++ port already made this choice, and this module was told to start from it,
/// not silently diverge further from an already-adopted reference without an explicit decision.
pub fn kmeans_plusplus_seed_indices<const D: usize>(
    observations: &[[f64; D]],
    k: usize,
    rng: &mut SplitMix64,
) -> Vec<usize> {
    let n = observations.len();
    if n == 0 || k == 0 {
        return Vec::new();
    }

    let mut seeds = Vec::with_capacity(k.min(n));
    seeds.push(rng.next_bounded(n as u64) as usize);

    let mut min_sq_dist = vec![f64::MAX; n];
    while seeds.len() < k && seeds.len() < n {
        let last = &observations[*seeds.last().expect("seeds is non-empty by the push above")];
        let mut total_weight = 0.0;
        for (obs, min_d) in observations.iter().zip(min_sq_dist.iter_mut()) {
            let d = squared_distance(obs, last);
            if d < *min_d {
                *min_d = d;
            }
            total_weight += *min_d;
        }

        if total_weight <= 0.0 {
            seeds.push(rng.next_bounded(n as u64) as usize);
            continue;
        }

        let target = rng.next_f64() * total_weight;
        let mut cumulative = 0.0;
        let mut chosen = n - 1;
        for (i, &min_d) in min_sq_dist.iter().enumerate() {
            cumulative += min_d;
            if cumulative >= target {
                chosen = i;
                break;
            }
        }
        seeds.push(chosen);
    }

    seeds
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    #[test]
    fn splitmix64_matches_reference_implementation() {
        // Reference: Vigna's own splitmix64.c, compiled and run directly with `x = 42` (gcc,
        // 2026-10-11) -- not reproduced from memory.
        let mut rng = SplitMix64::new(42);
        let want: [u64; 5] = [
            13679457532755275413,
            2949826092126892291,
            5139283748462763858,
            6349198060258255764,
            701532786141963250,
        ];
        for (i, &w) in want.iter().enumerate() {
            assert_eq!(rng.next_u64(), w, "splitmix64 output #{i} mismatch");
        }
    }

    #[test]
    fn next_bounded_stays_in_range_and_is_not_degenerate() {
        let mut rng = SplitMix64::new(7);
        let mut seen = std::collections::HashSet::new();
        for _ in 0..2000 {
            let v = rng.next_bounded(10);
            assert!(v < 10, "next_bounded(10) returned {v}");
            seen.insert(v);
        }
        // Loose sanity check, not a strict uniformity test: 2000 draws over 10 buckets should hit
        // every bucket at least once with overwhelming probability if the generator isn't
        // degenerate (e.g. always returning 0, or cycling trivially).
        assert_eq!(seen.len(), 10, "expected all 10 buckets to be hit at least once");
    }

    #[test]
    fn next_bounded_zero_never_panics() {
        let mut rng = SplitMix64::new(1);
        assert_eq!(rng.next_bounded(0), 0);
    }

    #[test]
    fn kmeans_plusplus_picks_k_distinct_indices() {
        let obs: Vec<[f64; 2]> = vec![
            [0.0, 0.0],
            [0.1, 0.1],
            [5.0, 5.0],
            [5.1, 5.1],
            [10.0, 0.0],
            [10.1, 0.1],
        ];
        let mut rng = SplitMix64::new(123);
        let seeds = kmeans_plusplus_seed_indices(&obs, 3, &mut rng);
        assert_eq!(seeds.len(), 3);
        let unique: std::collections::HashSet<usize> = seeds.iter().copied().collect();
        assert_eq!(unique.len(), 3, "k-means++ must return distinct indices: {seeds:?}");
        for &s in &seeds {
            assert!(s < obs.len());
        }
    }

    #[test]
    fn kmeans_plusplus_is_deterministic_for_a_fixed_seed() {
        let obs: Vec<[f64; 2]> = vec![[0.0, 0.0], [0.1, 0.1], [5.0, 5.0], [5.1, 5.1]];
        let seeds_a = kmeans_plusplus_seed_indices(&obs, 2, &mut SplitMix64::new(99));
        let seeds_b = kmeans_plusplus_seed_indices(&obs, 2, &mut SplitMix64::new(99));
        assert_eq!(seeds_a, seeds_b, "same seed must reproduce the same picks");
    }

    #[test]
    fn kmeans_plusplus_handles_fully_coincident_points() {
        // Degenerate case: every point identical -- total_weight collapses to 0.0 on the second
        // pick onward, must fall back to uniform sampling rather than divide-by-zero/NaN/panic.
        let obs: Vec<[f64; 3]> = vec![[1.0, 1.0, 1.0]; 5];
        let mut rng = SplitMix64::new(5);
        let seeds = kmeans_plusplus_seed_indices(&obs, 3, &mut rng);
        assert_eq!(seeds.len(), 3);
        for &s in &seeds {
            assert!(s < obs.len());
        }
    }

    #[test]
    fn kmeans_plusplus_favors_well_separated_clusters_over_many_trials() {
        // Statistical sanity check, not a bit-exact golden: two tight, far-apart clusters --
        // k-means++'s whole design goal is to avoid picking two seeds from the same cluster when
        // a well-separated alternative exists, so across many different seeds, "one seed per
        // cluster" should be the common outcome, not the rare one.
        let obs: Vec<[f64; 2]> = vec![
            [0.0, 0.0], [0.05, 0.02], [-0.03, 0.04], [0.02, -0.05],
            [100.0, 100.0], [100.05, 100.02], [99.97, 100.04], [100.02, 99.95],
        ];
        let mut one_per_cluster = 0;
        for seed in 0..200u64 {
            let mut rng = SplitMix64::new(seed);
            let seeds = kmeans_plusplus_seed_indices(&obs, 2, &mut rng);
            let (a, b) = (seeds[0] < 4, seeds[1] < 4);
            if a != b {
                one_per_cluster += 1;
            }
        }
        assert!(
            one_per_cluster > 150,
            "expected k-means++ to pick one seed per cluster in the clear majority of trials, got {one_per_cluster}/200"
        );
    }

    #[test]
    fn kmeans_plusplus_returns_fewer_than_k_when_not_enough_rows() {
        let obs: Vec<[f64; 2]> = vec![[0.0, 0.0], [1.0, 1.0]];
        let mut rng = SplitMix64::new(3);
        let seeds = kmeans_plusplus_seed_indices(&obs, 5, &mut rng);
        assert_eq!(seeds.len(), 2);
    }
}
