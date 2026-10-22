//! Send window for Pi -> Flipper data (docs/PROTOCOL.md, "Flow control").
//!
//! The Flipper restores the window only after it drops to exactly zero, so
//! `acquire` hands out chunks until the window is used up and then waits for a
//! notification.

use tokio::sync::watch;

#[derive(Clone)]
pub struct Window {
    tx: std::sync::Arc<watch::Sender<u32>>,
}

impl Window {
    pub fn new(initial: u32) -> Self {
        Self {
            tx: std::sync::Arc::new(watch::Sender::new(initial)),
        }
    }

    /// New window value from a FLOW characteristic notification.
    pub fn set(&self, value: u32) {
        self.tx.send_replace(value);
    }

    /// Waits for a non-zero window and reserves at most `max` bytes of it.
    pub async fn acquire(&self, max: usize) -> usize {
        let mut rx = self.tx.subscribe();
        loop {
            let mut taken = 0;
            self.tx.send_if_modified(|w| {
                if *w == 0 {
                    return false;
                }
                taken = (*w as usize).min(max);
                *w -= taken as u32;
                true
            });
            if taken > 0 {
                return taken;
            }
            // The sender lives in `self`, so `changed` cannot fail.
            let _ = rx.changed().await;
        }
    }

    #[cfg(test)]
    fn get(&self) -> u32 {
        *self.tx.borrow()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[tokio::test]
    async fn drains_window_to_zero() {
        let w = Window::new(50);
        assert_eq!(w.acquire(20).await, 20);
        assert_eq!(w.acquire(20).await, 20);
        assert_eq!(w.acquire(20).await, 10);
        assert_eq!(w.get(), 0);
    }

    #[tokio::test]
    async fn waits_for_refill() {
        let w = Window::new(0);
        let w2 = w.clone();
        let task = tokio::spawn(async move { w2.acquire(20).await });
        tokio::time::sleep(Duration::from_millis(20)).await;
        assert!(!task.is_finished());
        w.set(1024);
        assert_eq!(task.await.unwrap(), 20);
        assert_eq!(w.get(), 1004);
    }
}
