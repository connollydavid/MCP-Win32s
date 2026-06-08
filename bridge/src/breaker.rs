//! A device-facing circuit-breaker. The bridge talks to a single device over
//! one link; when that link is in a fault state, hammering it with every tool
//! call wastes a round-trip and delays recovery. The breaker counts
//! consecutive device failures (an `Err`/transport error or a recoverable
//! device error from the relay) and, at a threshold, opens: subsequent calls
//! are short-circuited locally with the recoverable "device circuit open"
//! result (`ToolCallCircuitOpen`) until a cooldown elapses, after which one
//! trial call is admitted (HalfOpen). A successful trial closes the breaker; a
//! failed one re-opens it for another cooldown.
//!
//! Time is injected (`Clock`) so the state machine is unit-tested with no real
//! sleeps. The threshold/cooldown are tuning, not spec-pinned behaviour
//! (OBLIGATIONS-5.5.md): the tests pin the refusal, not the policy.

use std::time::{Duration, Instant};

/// The breaker's observable state. `Open` short-circuits; `HalfOpen` admits one
/// trial; `Closed` is the normal pass-through.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum State {
    Closed,
    Open,
    HalfOpen,
}

/// A monotonic clock the breaker reads `now()` from. The production clock reads
/// the real monotonic time; tests inject a controllable one so transitions are
/// driven without sleeping.
pub trait Clock: Send {
    fn now(&self) -> Instant;
}

/// The real monotonic clock.
pub struct SystemClock;

impl Clock for SystemClock {
    fn now(&self) -> Instant {
        Instant::now()
    }
}

/// A device-facing circuit-breaker over an injectable clock.
pub struct Breaker {
    failure_threshold: u32,
    cooldown: Duration,
    clock: Box<dyn Clock>,
    consecutive_failures: u32,
    /// When `Some`, the breaker is open until this instant; on the first
    /// `admit` at or after it, the breaker advances to HalfOpen.
    open_until: Option<Instant>,
    half_open: bool,
}

impl Breaker {
    /// A breaker that opens after `failure_threshold` consecutive failures and
    /// stays open for `cooldown`, driven by `clock`.
    pub fn new(failure_threshold: u32, cooldown: Duration, clock: Box<dyn Clock>) -> Self {
        Breaker {
            failure_threshold: failure_threshold.max(1),
            cooldown,
            clock,
            consecutive_failures: 0,
            open_until: None,
            half_open: false,
        }
    }

    /// The production breaker: real clock, a small consecutive-failure
    /// threshold, a short cooldown. These are tuning values, not spec-pinned.
    pub fn production() -> Self {
        Breaker::new(5, Duration::from_secs(30), Box::new(SystemClock))
    }

    /// The breaker's current state, evaluated against the clock (an expired
    /// open window reads as HalfOpen without admitting a trial).
    pub fn state(&self) -> State {
        match self.open_until {
            None => State::Closed,
            Some(until) => {
                if self.half_open || self.clock.now() >= until {
                    State::HalfOpen
                } else {
                    State::Open
                }
            }
        }
    }

    /// Decide whether to admit a call. Returns `true` when the call may proceed
    /// to the device (Closed, or the single HalfOpen trial); `false` when the
    /// breaker is Open and the call must be short-circuited locally. Admitting a
    /// trial latches HalfOpen until `record_success`/`record_failure` resolves it.
    pub fn admit(&mut self) -> bool {
        match self.open_until {
            None => true,
            Some(until) => {
                if self.half_open {
                    // A trial is already in flight; do not admit a second.
                    false
                } else if self.clock.now() >= until {
                    // Cooldown elapsed: admit exactly one trial (HalfOpen).
                    self.half_open = true;
                    true
                } else {
                    false
                }
            }
        }
    }

    /// Record a successful device round-trip: closes the breaker.
    pub fn record_success(&mut self) {
        self.consecutive_failures = 0;
        self.open_until = None;
        self.half_open = false;
    }

    /// Record a device failure (transport error or a recoverable device error).
    /// In HalfOpen this re-opens immediately for another cooldown; in Closed it
    /// counts toward the threshold and opens when the threshold is reached.
    pub fn record_failure(&mut self) {
        if self.half_open {
            self.trip();
            return;
        }
        self.consecutive_failures = self.consecutive_failures.saturating_add(1);
        if self.consecutive_failures >= self.failure_threshold {
            self.trip();
        }
    }

    fn trip(&mut self) {
        self.open_until = Some(self.clock.now() + self.cooldown);
        self.half_open = false;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;
    use std::rc::Rc;

    /// A clock whose `now()` is whatever the test last set. `Rc<Cell<…>>` so the
    /// test holds a handle to advance it after the breaker took ownership.
    #[derive(Clone)]
    struct FakeClock {
        now: Rc<Cell<Instant>>,
    }

    // The breaker requires `Clock: Send`; the test is single-threaded, so this
    // never crosses a thread. Asserting Send keeps the trait bound honest.
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
    fn closed_passes_until_threshold() {
        let (clock, _h) = FakeClock::new();
        let mut b = Breaker::new(3, Duration::from_secs(10), Box::new(clock));
        assert_eq!(b.state(), State::Closed);
        assert!(b.admit());
        b.record_failure();
        assert!(b.admit(), "one failure does not open");
        b.record_failure();
        assert!(b.admit(), "two failures (below threshold) does not open");
        assert_eq!(b.state(), State::Closed);
    }

    #[test]
    fn opens_at_threshold() {
        let (clock, _h) = FakeClock::new();
        let mut b = Breaker::new(3, Duration::from_secs(10), Box::new(clock));
        b.record_failure();
        b.record_failure();
        b.record_failure();
        assert_eq!(b.state(), State::Open, "third failure opens the breaker");
        assert!(!b.admit(), "Open short-circuits");
    }

    #[test]
    fn success_resets_the_failure_count() {
        let (clock, _h) = FakeClock::new();
        let mut b = Breaker::new(3, Duration::from_secs(10), Box::new(clock));
        b.record_failure();
        b.record_failure();
        b.record_success();
        b.record_failure();
        b.record_failure();
        assert_eq!(b.state(), State::Closed, "the count reset on success");
    }

    #[test]
    fn open_to_half_open_after_cooldown() {
        let (clock, handle) = FakeClock::new();
        let mut b = Breaker::new(1, Duration::from_secs(10), Box::new(clock));
        b.record_failure();
        assert_eq!(b.state(), State::Open);
        assert!(!b.admit(), "still cooling down");

        advance(&handle, Duration::from_secs(10));
        assert_eq!(b.state(), State::HalfOpen, "cooldown elapsed");
        assert!(b.admit(), "the single trial is admitted");
        assert!(
            !b.admit(),
            "a second trial is NOT admitted while one is in flight"
        );
    }

    #[test]
    fn half_open_to_closed_on_success() {
        let (clock, handle) = FakeClock::new();
        let mut b = Breaker::new(1, Duration::from_secs(10), Box::new(clock));
        b.record_failure();
        advance(&handle, Duration::from_secs(10));
        assert!(b.admit(), "trial admitted");
        b.record_success();
        assert_eq!(
            b.state(),
            State::Closed,
            "a passing trial closes the breaker"
        );
        assert!(b.admit());
    }

    #[test]
    fn half_open_to_open_on_failure() {
        let (clock, handle) = FakeClock::new();
        let mut b = Breaker::new(1, Duration::from_secs(10), Box::new(clock));
        b.record_failure();
        advance(&handle, Duration::from_secs(10));
        assert!(b.admit(), "trial admitted");
        b.record_failure();
        assert_eq!(
            b.state(),
            State::Open,
            "a failing trial re-opens the breaker"
        );
        assert!(!b.admit(), "and the cooldown restarts");

        advance(&handle, Duration::from_secs(10));
        assert_eq!(b.state(), State::HalfOpen, "the second cooldown elapsed");
    }
}
