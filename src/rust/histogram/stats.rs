// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Statistical analysis of histograms, ported from Node.js's `src/histogram.cc`: distribution
//! shape, confidence intervals, hypothesis tests, and effect sizes.
//!
//! All of them work on the bucketed data, so they reflect the histogram's precision: values are
//! represented by their bucket midpoints, and values in the same bucket are ties.

#![expect(
    clippy::cast_precision_loss,
    reason = "counts and values are converted to f64 exactly where Node.js converts them to double"
)]
#![expect(
    clippy::suboptimal_flops,
    reason = "fused operations would round differently from Node.js"
)]

use std::f64::consts::PI;
use std::f64::consts::SQRT_2;

use crate::Histogram;

/// A confidence interval for the mean.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MeanCi {
    pub mean: f64,
    pub lower: f64,
    pub upper: f64,
}

/// The result of Welch's t-test.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WelchTest {
    pub t_statistic: f64,
    pub degrees_of_freedom: f64,
    /// Two-tailed.
    pub p_value: f64,
    /// The confidence interval on the difference of the means.
    pub ci_lower: f64,
    pub ci_upper: f64,
}

/// The result of the Mann-Whitney U test.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MannWhitneyTest {
    pub u_statistic: f64,
    pub z_score: f64,
    /// Two-tailed, from the normal approximation with tie correction.
    pub p_value: f64,
}

/// A confidence interval for a percentile.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PercentileCi {
    pub value: i64,
    pub lower: i64,
    pub upper: i64,
}

// Numerical helpers.

/// The continued fraction for the regularized incomplete beta function, by Lentz's modified
/// method. Reference: Numerical Recipes in C, 2nd edition, section 6.4.
#[expect(
    clippy::many_single_char_names,
    reason = "the names follow Numerical Recipes and Node.js"
)]
fn beta_continued_fraction(a: f64, b: f64, x: f64) -> f64 {
    const FPMIN: f64 = 1e-30;
    const MAXIT: i32 = 200;
    const EPS: f64 = 3e-12;

    let qab = a + b;
    let qap = a + 1.0;
    let qam = a - 1.0;
    let mut c = 1.0;
    let mut d = 1.0 - qab * x / qap;
    if d.abs() < FPMIN {
        d = FPMIN;
    }
    d = 1.0 / d;
    let mut h = d;

    for m in 1..=MAXIT {
        let m = f64::from(m);
        let m2 = 2.0 * m;
        // Even step.
        let mut aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if d.abs() < FPMIN {
            d = FPMIN;
        }
        c = 1.0 + aa / c;
        if c.abs() < FPMIN {
            c = FPMIN;
        }
        d = 1.0 / d;
        h *= d * c;
        // Odd step.
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if d.abs() < FPMIN {
            d = FPMIN;
        }
        c = 1.0 + aa / c;
        if c.abs() < FPMIN {
            c = FPMIN;
        }
        d = 1.0 / d;
        let del = d * c;
        h *= del;
        if (del - 1.0).abs() <= EPS {
            break;
        }
    }
    h
}

/// The shape of a beta distribution and the values derived from it that `I_x(a, b)` needs:
/// `BetaParameters` in Node.js.
#[derive(Debug, Clone, Copy)]
pub struct BetaParameters {
    pub a: f64,
    pub b: f64,
    log_normalization: f64,
    pub mean: f64,
    pub standard_deviation: f64,
    skewness: f64,
    excess_kurtosis: f64,
    log_scale: f64,
    pub symmetry_point: f64,
    use_asymptotic_front: bool,
    pub use_asymptotic_cdf: bool,
}

fn stirling_correction(x: f64) -> f64 {
    let inverse = 1.0 / x;
    let inverse_squared = inverse * inverse;
    inverse
        * (1.0 / 12.0
            + inverse_squared
                * (-1.0 / 360.0
                    + inverse_squared
                        * (1.0 / 1260.0
                            + inverse_squared * (-1.0 / 1680.0 + inverse_squared / 1188.0))))
}

impl BetaParameters {
    /// With `approximate_large_shapes`, very concentrated shapes use a Stirling approximation of
    /// the normalization and an Edgeworth expansion of the CDF, which are more accurate there
    /// than the continued fraction. Only QRDE asks for them.
    pub fn new(a: f64, b: f64, approximate_large_shapes: bool) -> Self {
        let sum = a + b;
        let mean = a / sum;
        let use_asymptotic_front = approximate_large_shapes && a >= 8.0 && b >= 8.0;
        // The continued fraction needs progressively more iterations as both shapes grow. At this
        // concentration the Edgeworth error is smaller than the continued-fraction truncation
        // error.
        let use_asymptotic_cdf = approximate_large_shapes && a * b / sum >= 25_000.0;
        let variance = mean * (1.0 - mean) / (sum + 1.0);
        let skewness = 2.0 * (b - a) * (sum + 1.0).sqrt() / ((sum + 2.0) * (a * b).sqrt());
        let excess_kurtosis = 6.0 * ((a - b) * (a - b) * (sum + 1.0) - a * b * (sum + 2.0))
            / (a * b * (sum + 2.0) * (sum + 3.0));
        Self {
            a,
            b,
            log_normalization: if use_asymptotic_front {
                0.0
            } else {
                libm::lgamma(sum) - libm::lgamma(a) - libm::lgamma(b)
            },
            mean,
            standard_deviation: variance.sqrt(),
            skewness,
            excess_kurtosis,
            log_scale: if use_asymptotic_front {
                0.5 * (sum * mean * (1.0 - mean) / (2.0 * PI)).ln() + stirling_correction(sum)
                    - stirling_correction(a)
                    - stirling_correction(b)
            } else {
                0.0
            },
            symmetry_point: (a + 1.0) / (sum + 2.0),
            use_asymptotic_front,
            use_asymptotic_cdf,
        }
    }

    /// The parameters of Beta(b, a), whose CDF at 1 - x is the survival function at x.
    pub fn reflect(&self) -> Self {
        Self {
            a: self.b,
            b: self.a,
            mean: 1.0 - self.mean,
            skewness: -self.skewness,
            symmetry_point: 1.0 - self.symmetry_point,
            ..*self
        }
    }

    /// `x^a (1 - x)^b / B(a, b)`.
    fn front(&self, x: f64) -> f64 {
        if x <= 0.0 || x >= 1.0 {
            return 0.0;
        }
        let log_front = if self.use_asymptotic_front {
            self.a * ((x - self.mean) / self.mean).ln_1p()
                + self.b * ((self.mean - x) / (1.0 - self.mean)).ln_1p()
                + self.log_scale
        } else {
            self.log_normalization + self.a * x.ln() + self.b * (1.0 - x).ln()
        };
        log_front.exp()
    }

    /// The CDF from an Edgeworth expansion around the normal approximation, and the front.
    #[expect(clippy::similar_names, reason = "the names follow Node.js")]
    fn asymptotic_cdf(&self, x: f64) -> (f64, f64) {
        let z = (x - self.mean) / self.standard_deviation;
        let normal_cdf = 0.5 * libm::erfc(-z / SQRT_2);
        if z.abs() >= 8.0 {
            return (normal_cdf, 0.0);
        }

        let z2 = z * z;
        let z3 = z2 * z;
        let normal_pdf = (-0.5 * z2).exp() / (2.0 * PI).sqrt();
        let z4 = z2 * z2;
        let z6 = z3 * z3;
        let density_correction = 1.0
            + self.skewness / 6.0 * (z3 - 3.0 * z)
            + self.excess_kurtosis / 24.0 * (z4 - 6.0 * z2 + 3.0)
            + self.skewness * self.skewness / 72.0 * (z6 - 15.0 * z4 + 45.0 * z2 - 15.0);
        let front =
            x * (1.0 - x) * normal_pdf / self.standard_deviation * std_max(0.0, density_correction);
        let correction = self.skewness / 6.0 * (1.0 - z2)
            - self.excess_kurtosis / 24.0 * (z3 - 3.0 * z)
            - self.skewness * self.skewness / 72.0 * (z3 * z2 - 10.0 * z3 + 15.0 * z);
        (
            std_clamp(normal_cdf + normal_pdf * correction, 0.0, 1.0),
            front,
        )
    }

    /// The regularized incomplete beta function `I_x(a, b)`, the probability that a Beta(a, b)
    /// random variable is at most `x`, and the front at `x` (0 outside (0, 1)).
    pub fn cdf_and_front(&self, x: f64) -> (f64, f64) {
        if x <= 0.0 {
            return (0.0, 0.0);
        }
        if x >= 1.0 {
            return (1.0, 0.0);
        }
        if self.use_asymptotic_cdf {
            return self.asymptotic_cdf(x);
        }
        let front = self.front(x);
        // The symmetry relation keeps the continued fraction in the region where it converges
        // best.
        let cdf = if x < self.symmetry_point {
            front * beta_continued_fraction(self.a, self.b, x) / self.a
        } else {
            1.0 - front * beta_continued_fraction(self.b, self.a, 1.0 - x) / self.b
        };
        (cdf, front)
    }

    pub fn cdf(&self, x: f64) -> f64 {
        self.cdf_and_front(x).0
    }
}

/// `std::clamp()`: unlike `f64::clamp()`, never panics, and returns `v` when it is NaN.
pub fn std_clamp(v: f64, lo: f64, hi: f64) -> f64 {
    if v < lo {
        lo
    } else if hi < v {
        hi
    } else {
        v
    }
}

/// `std::max()`: returns `a` unless `a < b`.
pub fn std_max(a: f64, b: f64) -> f64 {
    if a < b { b } else { a }
}

/// `std::min()`: returns `a` unless `b < a`.
pub fn std_min(a: f64, b: f64) -> f64 {
    if b < a { b } else { a }
}

/// The regularized incomplete beta function `I_x(a, b)`, computed exactly.
fn regularized_incomplete_beta(a: f64, b: f64, x: f64) -> f64 {
    BetaParameters::new(a, b, false).cdf(x)
}

/// The standard normal CDF: P(Z <= x).
fn normal_cdf(x: f64) -> f64 {
    0.5 * libm::erfc(-x * SQRT_2 / 2.0)
}

/// The CDF of Student's t-distribution with `df` degrees of freedom: P(T <= t).
fn student_t_cdf(t: f64, df: f64) -> f64 {
    let squared = t * t;
    let denominator = df + squared;
    if squared < df {
        let x = squared / denominator;
        let ibeta = regularized_incomplete_beta(0.5, df / 2.0, x);
        return if t >= 0.0 {
            0.5 + 0.5 * ibeta
        } else {
            0.5 - 0.5 * ibeta
        };
    }
    let x = df / denominator;
    let ibeta = regularized_incomplete_beta(df / 2.0, 0.5, x);
    if t >= 0.0 {
        1.0 - 0.5 * ibeta
    } else {
        0.5 * ibeta
    }
}

/// The positive t quantile with upper-tail probability `p`, by bisection on the lower tail, which
/// avoids losing precision for probabilities near 1.
#[expect(
    clippy::while_float,
    reason = "the bound doubles until it brackets the quantile, which takes few steps"
)]
#[expect(
    clippy::manual_midpoint,
    reason = "f64::midpoint rounds differently from Node.js's (lo + hi) / 2 for some inputs"
)]
fn student_t_upper_quantile(p: f64, df: f64) -> f64 {
    let mut lo = 0.0;
    let mut hi = 1.0;
    while student_t_cdf(-hi, df) > p {
        hi *= 2.0;
    }
    for _ in 0..100 {
        let mid = (lo + hi) / 2.0;
        if student_t_cdf(-mid, df) > p {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    (lo + hi) / 2.0
}

/// The binomial CDF P(X <= k) for X ~ Binomial(n, p), from P(X <= k) = I_{1-p}(n - k, k + 1).
fn binomial_cdf(k: i64, n: i64, p: f64) -> f64 {
    if k < 0 {
        return 0.0;
    }
    if k >= n {
        return 1.0;
    }
    regularized_incomplete_beta((n - k) as f64, (k + 1) as f64, 1.0 - p)
}

/// The counts of two histograms paired by index, over the longer of the two counts arrays.
fn paired_counts<'a>(a: &'a Histogram, b: &'a Histogram) -> impl Iterator<Item = (i64, i64)> + 'a {
    let a = a.hdr.counts();
    let b = b.hdr.counts();
    (0..a.len().max(b.len())).map(move |i| {
        (
            a.get(i).copied().unwrap_or(0),
            b.get(i).copied().unwrap_or(0),
        )
    })
}

/// The pairs of values from `a` and `b` where `a`'s is greater, and where they are tied, from one
/// walk over the paired counts.
fn concordant_and_tied(a: &Histogram, b: &Histogram) -> (f64, f64) {
    let mut cum2 = 0_i64;
    let mut concordant = 0.0;
    let mut tied = 0.0;
    for (c1, c2) in paired_counts(a, b) {
        concordant += c1 as f64 * cum2 as f64;
        tied += c1 as f64 * c2 as f64;
        cum2 += c2;
    }
    (concordant, tied)
}

/// The population variance converted to the sample variance (Bessel's correction).
fn sample_variance(stddev: f64, n: i64) -> f64 {
    stddev * stddev * n as f64 / (n - 1) as f64
}

impl Histogram {
    /// The skewness of the bucket midpoints: positive for a longer right tail. 0 with fewer than 3
    /// values or no variance.
    pub fn skewness(&self) -> f64 {
        let total = self.hdr.total_count();
        if total < 3 {
            return 0.0;
        }
        let mean = self.hdr.mean();
        let mut m2 = 0.0;
        let mut m3 = 0.0;
        for bucket in self.hdr.recorded() {
            let dev = self.hdr.median_equivalent_value(bucket.value) as f64 - mean;
            let d2 = dev * dev;
            m2 += bucket.count as f64 * d2;
            m3 += bucket.count as f64 * d2 * dev;
        }
        let n = total as f64;
        let variance = m2 / n;
        if variance == 0.0 {
            return 0.0;
        }
        let s3 = variance * variance.sqrt();
        (m3 / n) / s3
    }

    /// The excess kurtosis of the bucket midpoints: positive for heavier tails than a normal
    /// distribution. 0 with fewer than 4 values or no variance.
    pub fn kurtosis(&self) -> f64 {
        let total = self.hdr.total_count();
        if total < 4 {
            return 0.0;
        }
        let mean = self.hdr.mean();
        let mut m2 = 0.0;
        let mut m4 = 0.0;
        for bucket in self.hdr.recorded() {
            let dev = self.hdr.median_equivalent_value(bucket.value) as f64 - mean;
            let d2 = dev * dev;
            m2 += bucket.count as f64 * d2;
            m4 += bucket.count as f64 * d2 * d2;
        }
        let n = total as f64;
        let variance = m2 / n;
        if variance == 0.0 {
            return 0.0;
        }
        let s4 = variance * variance;
        (m4 / n) / s4 - 3.0
    }

    /// A two-sided confidence interval for the mean from Student's t-distribution. The bounds
    /// are NaN with fewer than 2 values, and equal to the mean when all values are equal.
    pub fn mean_ci(&self, confidence: f64) -> MeanCi {
        let count = self.hdr.total_count();
        let mean = self.hdr.mean();
        if count < 2 {
            return MeanCi {
                mean,
                lower: f64::NAN,
                upper: f64::NAN,
            };
        }
        let stddev = self.hdr.stddev();
        if stddev == 0.0 {
            return MeanCi {
                mean,
                lower: mean,
                upper: mean,
            };
        }
        let variance = sample_variance(stddev, count);
        let standard_error = (variance / count as f64).sqrt();
        let alpha = 1.0 - confidence;
        let t_crit = student_t_upper_quantile(alpha / 2.0, (count - 1) as f64);
        let margin = t_crit * standard_error;
        MeanCi {
            mean,
            lower: mean - margin,
            upper: mean + margin,
        }
    }

    /// A distribution-free confidence interval for a percentile, from the exact binomial
    /// distribution of order statistics. With fewer than 2 values, the bounds equal the value.
    pub fn percentile_ci(&self, percentile: f64, confidence: f64) -> PercentileCi {
        let value = self.hdr.value_at_percentile(percentile);
        let n = self.hdr.total_count();
        if n < 2 {
            return PercentileCi {
                value,
                lower: value,
                upper: value,
            };
        }

        let p = percentile / 100.0;
        let alpha = 1.0 - confidence;

        // Lower rank: the largest j with BinomialCdf(j - 1, n, p) <= alpha / 2.
        let mut lo = 0_i64;
        let mut hi = n;
        while lo < hi {
            let mid = lo + (hi - lo + 1) / 2;
            if binomial_cdf(mid - 1, n, p) <= alpha / 2.0 {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        let lower_pct = lo as f64 / n as f64 * 100.0;

        // Upper rank: the smallest k with BinomialCdf(k - 1, n, p) >= 1 - alpha / 2.
        lo = 0;
        hi = n;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if binomial_cdf(mid - 1, n, p) >= 1.0 - alpha / 2.0 {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        let upper_pct = lo as f64 / n as f64 * 100.0;

        PercentileCi {
            value,
            lower: self.hdr.value_at_percentile(lower_pct),
            upper: self.hdr.value_at_percentile(upper_pct),
        }
    }

    /// Welch's t-test comparing the means of this histogram and `other`. With fewer than 2 values
    /// in either, or no variance, the result has p-value 1 and every other field 0.
    pub fn welch_test(&self, other: &Self, confidence: f64) -> WelchTest {
        const NO_RESULT: WelchTest = WelchTest {
            t_statistic: 0.0,
            degrees_of_freedom: 0.0,
            p_value: 1.0,
            ci_lower: 0.0,
            ci_upper: 0.0,
        };
        if std::ptr::eq(self, other) {
            return NO_RESULT;
        }
        let n1 = self.hdr.total_count();
        let n2 = other.hdr.total_count();
        if n1 < 2 || n2 < 2 {
            return NO_RESULT;
        }

        let mean1 = self.hdr.mean();
        let mean2 = other.hdr.mean();
        let var1 = sample_variance(self.hdr.stddev(), n1);
        let var2 = sample_variance(other.hdr.stddev(), n2);

        let se1 = var1 / n1 as f64;
        let se2 = var2 / n2 as f64;
        let se_sum = se1 + se2;
        if se_sum == 0.0 {
            return NO_RESULT;
        }

        let t = (mean1 - mean2) / se_sum.sqrt();
        // Welch-Satterthwaite degrees of freedom.
        let df = (se_sum * se_sum) / (se1 * se1 / (n1 - 1) as f64 + se2 * se2 / (n2 - 1) as f64);
        let p = 2.0 * student_t_cdf(-t.abs(), df);

        let alpha = 1.0 - confidence;
        let t_crit = student_t_upper_quantile(alpha / 2.0, df);
        let margin = t_crit * se_sum.sqrt();
        let diff = mean1 - mean2;

        WelchTest {
            t_statistic: t,
            degrees_of_freedom: df,
            p_value: p,
            ci_lower: diff - margin,
            ci_upper: diff + margin,
        }
    }

    /// The Mann-Whitney U test of whether this histogram tends to hold larger or smaller values
    /// than `other`. Values in the same bucket are ties.
    pub fn mann_whitney_test(&self, other: &Self) -> MannWhitneyTest {
        const NO_RESULT: MannWhitneyTest = MannWhitneyTest {
            u_statistic: 0.0,
            z_score: 0.0,
            p_value: 1.0,
        };
        if std::ptr::eq(self, other) {
            return NO_RESULT;
        }
        let n1 = self.hdr.total_count();
        let n2 = other.hdr.total_count();
        if n1 == 0 || n2 == 0 {
            return NO_RESULT;
        }

        // Values of `self` at index i exceed every value of `other` below index i.
        let (concordant, tied) = concordant_and_tied(self, other);
        let u = concordant + 0.5 * tied;
        let dn1 = n1 as f64;
        let dn2 = n2 as f64;
        let mu = dn1 * dn2 / 2.0;

        // sigma^2 = n1 * n2 / 12 * (N + 1 - sum(t^3 - t) / (N * (N - 1))), where t is the number
        // of values tied in a bucket.
        let n_total = dn1 + dn2;
        let mut tie_correction = 0.0;
        for (c1, c2) in paired_counts(self, other) {
            let tk = (c1 + c2) as f64;
            if tk > 1.0 {
                tie_correction += tk * tk * tk - tk;
            }
        }
        let sigma_sq =
            (dn1 * dn2 / 12.0) * (n_total + 1.0 - tie_correction / (n_total * (n_total - 1.0)));
        if sigma_sq <= 0.0 {
            return MannWhitneyTest {
                u_statistic: u,
                z_score: 0.0,
                p_value: 1.0,
            };
        }

        let z = (u - mu) / sigma_sq.sqrt();
        MannWhitneyTest {
            u_statistic: u,
            z_score: z,
            p_value: 2.0 * normal_cdf(-z.abs()),
        }
    }

    /// Cohen's d: the difference of the means in units of the pooled sample standard deviation.
    /// 0 with fewer than 2 values in either histogram or no variance.
    pub fn cohens_d(&self, other: &Self) -> f64 {
        if std::ptr::eq(self, other) {
            return 0.0;
        }
        let n1 = self.hdr.total_count();
        let n2 = other.hdr.total_count();
        if n1 < 2 || n2 < 2 {
            return 0.0;
        }
        let mean1 = self.hdr.mean();
        let mean2 = other.hdr.mean();
        let var1 = sample_variance(self.hdr.stddev(), n1);
        let var2 = sample_variance(other.hdr.stddev(), n2);
        let pooled_sd =
            (((n1 - 1) as f64 * var1 + (n2 - 1) as f64 * var2) / (n1 + n2 - 2) as f64).sqrt();
        if pooled_sd == 0.0 {
            return 0.0;
        }
        (mean1 - mean2) / pooled_sd
    }

    /// Cliff's delta: the probability that a value of this histogram exceeds a value of `other`,
    /// minus the reverse, in [-1, 1].
    pub fn cliffs_d(&self, other: &Self) -> f64 {
        if std::ptr::eq(self, other) {
            return 0.0;
        }
        let n1 = self.hdr.total_count();
        let n2 = other.hdr.total_count();
        if n1 == 0 || n2 == 0 {
            return 0.0;
        }
        let (concordant, tied) = concordant_and_tied(self, other);
        let pairs = n1 as f64 * n2 as f64;
        let discordant = pairs - concordant - tied;
        (concordant - discordant) / pairs
    }

    /// The Kolmogorov-Smirnov statistic: the largest difference between the two cumulative
    /// distributions, in [0, 1].
    pub fn ks_test(&self, other: &Self) -> f64 {
        if std::ptr::eq(self, other) {
            return 0.0;
        }
        let n1 = self.hdr.total_count();
        let n2 = other.hdr.total_count();
        if n1 == 0 || n2 == 0 {
            return 0.0;
        }
        let mut max_d = 0.0_f64;
        let mut cum1 = 0_i64;
        let mut cum2 = 0_i64;
        for (c1, c2) in paired_counts(self, other) {
            cum1 += c1;
            cum2 += c2;
            let cdf1 = cum1 as f64 / n1 as f64;
            let cdf2 = cum2 as f64 / n2 as f64;
            let d = if cdf1 > cdf2 {
                cdf1 - cdf2
            } else {
                cdf2 - cdf1
            };
            if d > max_d {
                max_d = d;
            }
        }
        max_d
    }
}

#[cfg(test)]
#[path = "stats-test.rs"]
mod tests;
