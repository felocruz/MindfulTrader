//! Fixed-capacity rolling windows (spec §6.2.2 principle 3): allocated once, never again.
//!
//! [`WindowRing`] keeps the most recent `N` values and lends the last `n` of them as ONE contiguous
//! slice, oldest first, so the `observation_vector` formulas (which take `&[f32]`/`&[f64]`) run on the
//! window in place: every value is written twice, at `pos` and `pos + N` of a `2N` buffer, which
//! makes `buf[pos + N - n .. pos + N]` the last `n` values at all times. Cost: two stores per push,
//! instead of a copy of the window per read.
//!
//! [`SortedWindow`] keeps the same values in sorted order as well, for rolling median and MAD: each
//! push is two binary searches and two `memmove`s over at most `N` values; median is O(1) and MAD a
//! single outward walk from the median.

/// The most recent `N` values, readable as one contiguous slice.
#[derive(Debug, Clone)]
pub struct WindowRing<T: Copy + Default, const N: usize> {
    buf: Box<[T]>,
    /// Next write position, in `0..N`.
    pos: usize,
    len: usize,
}

impl<T: Copy + Default, const N: usize> Default for WindowRing<T, N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T: Copy + Default, const N: usize> WindowRing<T, N> {
    const CAPACITY_IS_POSITIVE: () = assert!(N > 0, "WindowRing capacity must be positive");

    pub fn new() -> Self {
        let () = Self::CAPACITY_IS_POSITIVE;
        Self { buf: vec![T::default(); 2 * N].into_boxed_slice(), pos: 0, len: 0 }
    }

    pub const fn capacity(&self) -> usize {
        N
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn is_full(&self) -> bool {
        self.len == N
    }

    /// Appends a value, evicting the oldest when full.
    #[inline]
    pub fn push(&mut self, value: T) {
        self.buf[self.pos] = value;
        self.buf[self.pos + N] = value;
        self.pos = if self.pos + 1 == N { 0 } else { self.pos + 1 };
        if self.len < N {
            self.len += 1;
        }
    }

    /// The last `n` values, oldest first, as one slice; `None` if fewer than `n` are held.
    #[inline]
    pub fn last(&self, n: usize) -> Option<&[T]> {
        if n > self.len {
            return None;
        }
        let end = self.pos + N;
        Some(&self.buf[end - n..end])
    }

    /// Every held value, oldest first.
    pub fn as_slice(&self) -> &[T] {
        let end = self.pos + N;
        &self.buf[end - self.len..end]
    }

    /// The newest value.
    pub fn newest(&self) -> Option<T> {
        self.as_slice().last().copied()
    }

    /// The oldest value (the next to be evicted once full).
    pub fn oldest(&self) -> Option<T> {
        self.as_slice().first().copied()
    }

    pub fn clear(&mut self) {
        self.pos = 0;
        self.len = 0;
    }
}

/// A rolling window of the last `N` finite values, also held in sorted order.
#[derive(Debug, Clone)]
pub struct SortedWindow<const N: usize> {
    chrono: WindowRing<f64, N>,
    sorted: Vec<f64>,
}

impl<const N: usize> Default for SortedWindow<N> {
    fn default() -> Self {
        Self::new()
    }
}

/// A non-finite value was offered to a [`SortedWindow`]; the window is unchanged.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct NonFinite(pub f64);

impl<const N: usize> SortedWindow<N> {
    pub fn new() -> Self {
        Self { chrono: WindowRing::new(), sorted: Vec::with_capacity(N) }
    }

    pub fn len(&self) -> usize {
        self.sorted.len()
    }

    pub fn is_empty(&self) -> bool {
        self.sorted.is_empty()
    }

    pub fn is_full(&self) -> bool {
        self.chrono.is_full()
    }

    /// The window in arrival order, oldest first.
    pub fn values(&self) -> &[f64] {
        self.chrono.as_slice()
    }

    /// The window in ascending order.
    pub fn sorted(&self) -> &[f64] {
        &self.sorted
    }

    /// Appends a value, evicting the oldest when full.
    pub fn push(&mut self, value: f64) -> Result<(), NonFinite> {
        if !value.is_finite() {
            return Err(NonFinite(value));
        }
        if self.chrono.is_full()
            && let Some(old) = self.chrono.oldest()
        {
            let at = self.sorted.partition_point(|v| v.total_cmp(&old).is_lt());
            // `old` is in the window by construction; total_cmp equality of finite values is bit equality.
            if at < self.sorted.len() && self.sorted[at].to_bits() == old.to_bits() {
                self.sorted.remove(at);
            }
        }
        let at = self.sorted.partition_point(|v| v.total_cmp(&value).is_lt());
        self.sorted.insert(at, value);
        self.chrono.push(value);
        Ok(())
    }

    /// The sample median (mean of the two middle values for an even count); `None` when empty.
    pub fn median(&self) -> Option<f64> {
        median_of_sorted(&self.sorted)
    }

    /// The median absolute deviation from the median (unscaled); `None` when empty.
    pub fn mad(&self) -> Option<f64> {
        let a = &self.sorted;
        let n = a.len();
        let m = median_of_sorted(a)?;
        // Distances to the median grow outward from it on both sides: merge the two ascending runs
        // and stop at the middle rank(s).
        let mut r = a.partition_point(|v| *v < m);
        let mut l = r; // elements a[..l] are below m, walked downward
        let (lo_rank, hi_rank) = ((n - 1) / 2, n / 2);
        let mut lo_value = 0.0;
        for rank in 0..=hi_rank {
            let left = if l > 0 { Some(m - a[l - 1]) } else { None };
            let right = if r < n { Some(a[r] - m) } else { None };
            let d = match (left, right) {
                (Some(x), Some(y)) if x <= y => {
                    l -= 1;
                    x
                }
                (_, Some(y)) => {
                    r += 1;
                    y
                }
                (Some(x), None) => {
                    l -= 1;
                    x
                }
                (None, None) => return None,
            };
            if rank == lo_rank {
                lo_value = d;
            }
            if rank == hi_rank {
                return Some((lo_value + d) / 2.0);
            }
        }
        None
    }

    pub fn clear(&mut self) {
        self.chrono.clear();
        self.sorted.clear();
    }
}

fn median_of_sorted(a: &[f64]) -> Option<f64> {
    let n = a.len();
    if n == 0 {
        return None;
    }
    Some(if n % 2 == 1 { a[n / 2] } else { (a[n / 2 - 1] + a[n / 2]) / 2.0 })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Deterministic test values (xorshift64*), some deliberately tied.
    fn values(seed: u64, n: usize) -> Vec<f64> {
        let mut s = seed | 1;
        (0..n)
            .map(|_| {
                s ^= s >> 12;
                s ^= s << 25;
                s ^= s >> 27;
                let u = s.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 11;
                // quantized to quarter units so ties are common, like prices on a tick grid
                ((u % 400) as f64) * 0.25 - 50.0
            })
            .collect()
    }

    fn brute_median(v: &[f64]) -> f64 {
        let mut s = v.to_vec();
        s.sort_by(f64::total_cmp);
        median_of_sorted(&s).unwrap_or(f64::NAN)
    }

    fn brute_mad(v: &[f64]) -> f64 {
        let m = brute_median(v);
        let d: Vec<f64> = v.iter().map(|x| (x - m).abs()).collect();
        brute_median(&d)
    }

    #[test]
    fn window_ring_lends_the_last_n_contiguously_across_wraparound() {
        let mut r: WindowRing<i32, 5> = WindowRing::new();
        assert!(r.is_empty());
        assert_eq!(r.last(1), None);
        for x in 1..=3 {
            r.push(x);
        }
        assert_eq!(r.as_slice(), &[1, 2, 3]);
        assert_eq!(r.last(2), Some(&[2, 3][..]));
        assert_eq!(r.last(4), None);
        for x in 4..=12 {
            r.push(x);
        }
        assert!(r.is_full());
        assert_eq!(r.as_slice(), &[8, 9, 10, 11, 12]);
        assert_eq!(r.last(3), Some(&[10, 11, 12][..]));
        assert_eq!((r.oldest(), r.newest()), (Some(8), Some(12)));
        r.clear();
        assert!(r.is_empty());
        r.push(7);
        assert_eq!(r.as_slice(), &[7]);
    }

    #[test]
    fn window_ring_matches_a_naive_deque_for_every_length() {
        let mut r: WindowRing<f64, 7> = WindowRing::new();
        let mut naive: std::collections::VecDeque<f64> = std::collections::VecDeque::new();
        for (i, x) in values(3, 50).into_iter().enumerate() {
            r.push(x);
            naive.push_back(x);
            if naive.len() > 7 {
                naive.pop_front();
            }
            let expect: Vec<f64> = naive.iter().copied().collect();
            assert_eq!(r.as_slice().iter().map(|v| v.to_bits()).collect::<Vec<_>>(), expect.iter().map(|v| v.to_bits()).collect::<Vec<_>>(), "step {i}");
        }
    }

    #[test]
    fn sorted_window_median_and_mad_match_brute_force() {
        for (seed, n) in [(1u64, 200usize), (2, 37), (3, 1000)] {
            let mut w: SortedWindow<31> = SortedWindow::new();
            let mut naive: std::collections::VecDeque<f64> = std::collections::VecDeque::new();
            for (i, x) in values(seed, n).into_iter().enumerate() {
                w.push(x).expect("finite");
                naive.push_back(x);
                if naive.len() > 31 {
                    naive.pop_front();
                }
                let v: Vec<f64> = naive.iter().copied().collect();
                let (m, d) = (w.median().unwrap_or(f64::NAN), w.mad().unwrap_or(f64::NAN));
                assert_eq!(m.to_bits(), brute_median(&v).to_bits(), "median seed {seed} step {i}");
                assert_eq!(d.to_bits(), brute_mad(&v).to_bits(), "mad seed {seed} step {i}");
                assert!(w.sorted().windows(2).all(|p| p[0] <= p[1]));
                assert_eq!(w.values().len(), w.sorted().len());
            }
        }
    }

    #[test]
    fn sorted_window_known_answers_and_rejects_non_finite() {
        let mut w: SortedWindow<4> = SortedWindow::new();
        assert_eq!(w.median(), None);
        assert_eq!(w.mad(), None);
        for x in [1.0, 2.0, 3.0, 4.0] {
            w.push(x).expect("finite");
        }
        // median 2.5; deviations 1.5, 0.5, 0.5, 1.5 -> MAD 1.0
        assert_eq!(w.median(), Some(2.5));
        assert_eq!(w.mad(), Some(1.0));
        w.push(100.0).expect("finite"); // evicts 1.0 -> {2,3,4,100}
        assert_eq!(w.median(), Some(3.5));
        assert_eq!(w.mad(), Some(1.0)); // deviations 1.5, 0.5, 0.5, 96.5
        assert_eq!(w.push(f64::NAN).map_err(|e| e.0.is_nan()), Err(true));
        assert_eq!(w.push(f64::INFINITY), Err(NonFinite(f64::INFINITY)));
        assert_eq!(w.len(), 4);
        let mut c: SortedWindow<3> = SortedWindow::new();
        for x in [5.0, 5.0, 5.0] {
            c.push(x).expect("finite");
        }
        assert_eq!((c.median(), c.mad()), (Some(5.0), Some(0.0)));
    }
}
