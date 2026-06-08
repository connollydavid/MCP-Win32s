//! A per-tool token-bucket rate limiter. Each tool name gets its own bucket;
//! a call spends one token. When a bucket is exhausted the call is shed locally
//! with the recoverable "rate limited" result (`ToolCallRateLimited`) and never
//! reaches the device. Tokens refill at a fixed rate up to the bucket capacity.
//!
//! Time is injected (`Clock`, shared with the breaker) so refill is unit-tested
//! deterministically with no real sleeps. The cap/refill values are tuning, not
//! spec-pinned (OBLIGATIONS-5.5.md): the tests pin the shed behaviour.

use crate::breaker::{Clock, SystemClock};
use std::collections::HashMap;
use std::time::{Duration, Instant};

/// One tool's token bucket: integer tokens (no floating point), refilled by one
/// token every `refill_interval` up to `capacity`.
struct Bucket {
    capacity: u32,
    tokens: u32,
    refill_interval: Duration,
    last_refill: Instant,
}

impl Bucket {
    fn new(capacity: u32, refill_interval: Duration, now: Instant) -> Self {
        Bucket {
            capacity,
            tokens: capacity,
            refill_interval,
            last_refill: now,
        }
    }

    /// Add the whole tokens accrued since the last refill, advancing the
    /// reference instant by exactly the consumed whole intervals (so fractional
    /// time is not lost). No-op when the interval is zero.
    fn refill(&mut self, now: Instant) {
        if self.refill_interval.is_zero() || self.tokens >= self.capacity {
            return;
        }
        let elapsed = now.saturating_duration_since(self.last_refill);
        let intervals = (elapsed.as_nanos() / self.refill_interval.as_nanos().max(1)) as u64;
        if intervals == 0 {
            return;
        }
        let gained = intervals.min(u64::from(self.capacity - self.tokens)) as u32;
        self.tokens += gained;
        self.last_refill += self.refill_interval * (intervals as u32);
    }
}

/// A per-tool token-bucket rate limiter over an injectable clock. Buckets are
/// created lazily on first sight of a tool name, all with the same cap/refill.
pub struct RateLimiter {
    capacity: u32,
    refill_interval: Duration,
    clock: Box<dyn Clock>,
    buckets: HashMap<String, Bucket>,
}

impl RateLimiter {
    /// A limiter giving each tool `capacity` tokens, refilling one every
    /// `refill_interval`, driven by `clock`.
    pub fn new(capacity: u32, refill_interval: Duration, clock: Box<dyn Clock>) -> Self {
        RateLimiter {
            capacity: capacity.max(1),
            refill_interval,
            clock,
            buckets: HashMap::new(),
        }
    }

    /// The production limiter: real clock, a generous per-tool burst, a steady
    /// refill. Tuning values, not spec-pinned.
    pub fn production() -> Self {
        RateLimiter::new(60, Duration::from_secs(1), Box::new(SystemClock))
    }

    /// Try to spend one token for `tool`. Returns `true` when the call may
    /// proceed (a token was available and consumed); `false` when the bucket is
    /// exhausted and the call must be shed locally.
    pub fn try_acquire(&mut self, tool: &str) -> bool {
        let now = self.clock.now();
        let cap = self.capacity;
        let interval = self.refill_interval;
        let bucket = self
            .buckets
            .entry(tool.to_string())
            .or_insert_with(|| Bucket::new(cap, interval, now));
        bucket.refill(now);
        if bucket.tokens > 0 {
            bucket.tokens -= 1;
            true
        } else {
            false
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;
    use std::rc::Rc;

    #[derive(Clone)]
    struct FakeClock {
        now: Rc<Cell<Instant>>,
    }

    // Single-threaded test only; never crosses a thread.
    unsafe impl Send for FakeClock {}

    impl FakeClock {
        fn new() -> (Self, Rc<Cell<Instant>>) {
            let cell = Rc::new(Cell::new(Instant::now()));
            (FakeClock { now: cell.clone() }, cell)
        }
    }

    impl Clock for FakeClock {
        fn now(&self) -> Instant {
            self.now.get()
        }
    }

    fn advance(cell: &Rc<Cell<Instant>>, by: Duration) {
        cell.set(cell.get() + by);
    }

    #[test]
    fn bucket_exhausts_after_capacity() {
        let (clock, _h) = FakeClock::new();
        let mut rl = RateLimiter::new(3, Duration::from_secs(1), Box::new(clock));
        assert!(rl.try_acquire("win32_exec"));
        assert!(rl.try_acquire("win32_exec"));
        assert!(rl.try_acquire("win32_exec"));
        assert!(
            !rl.try_acquire("win32_exec"),
            "the fourth call is shed (bucket empty)"
        );
    }

    #[test]
    fn refills_over_time() {
        let (clock, handle) = FakeClock::new();
        let mut rl = RateLimiter::new(2, Duration::from_secs(1), Box::new(clock));
        assert!(rl.try_acquire("t"));
        assert!(rl.try_acquire("t"));
        assert!(!rl.try_acquire("t"), "exhausted");

        advance(&handle, Duration::from_secs(1));
        assert!(rl.try_acquire("t"), "one token refilled after one interval");
        assert!(!rl.try_acquire("t"), "only one refilled");

        advance(&handle, Duration::from_secs(5));
        // Refill is capped at capacity (2), not the 5 intervals elapsed.
        assert!(rl.try_acquire("t"));
        assert!(rl.try_acquire("t"));
        assert!(!rl.try_acquire("t"), "refill never exceeds capacity");
    }

    #[test]
    fn buckets_are_per_tool() {
        let (clock, _h) = FakeClock::new();
        let mut rl = RateLimiter::new(1, Duration::from_secs(1), Box::new(clock));
        assert!(rl.try_acquire("a"));
        assert!(!rl.try_acquire("a"), "a is exhausted");
        assert!(rl.try_acquire("b"), "b has its own independent bucket");
    }
}
