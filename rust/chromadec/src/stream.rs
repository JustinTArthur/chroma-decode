// SPDX-License-Identifier: GPL-3.0-or-later

//! Async [`Stream`] adapter over the batch decoder, gated behind the `tokio`
//! feature.
//!
//! [`chd_decode_frames_async`] is a single blocking call that fans out across
//! the library's own worker pool, so it can't be polled incrementally. This
//! adapter runs that blocking call on a [`spawn_blocking`] thread and forwards
//! each decoded frame through a bounded channel, yielding a [`FrameStream`]
//! the caller can `.await`. By default frames arrive in completion order — the
//! same contract as [`Decoder::decode_frames`]; [`FrameOrder::Indexed`] instead
//! yields them in requested-index order.
//!
//! The channel is bounded, so a slow consumer applies backpressure: the C
//! worker threads park on a full channel rather than buffering frames without
//! limit. Dropping the stream (or its receiver going away) requests
//! cancellation, so a decode in flight stops promptly.
//!
//! [`chd_decode_frames_async`]: crate::sys::chd_decode_frames_async
//! [`Stream`]: futures_core::Stream
//! [`spawn_blocking`]: tokio::task::spawn_blocking

use std::pin::Pin;
use std::sync::{Arc, Condvar, Mutex};
use std::task::{Context, Poll};

use tokio::sync::mpsc;
use tokio::task::JoinHandle;

use crate::cancel::Cancel;
use crate::decoder::Decoder;
use crate::error::{Error, Result, Status};
use crate::frame::Frame;
use crate::types::DecoderKind;
use crate::video::Video;

/// Default [`decode_frames_stream`] channel depth.
///
/// At 4:4:4 a decoded SD frame runs ~1–8 MB (float PAL being the largest), so
/// 8 buffered frames caps channel-held memory in the tens of MB — trivial on
/// any modern machine — while keeping the compute-bound worker pool saturated.
pub const DEFAULT_CHANNEL_DEPTH: usize = 8;

/// One item yielded by [`FrameStream`]: a frame index paired with its decode
/// result.
#[derive(Debug)]
pub struct DecodedFrame {
    /// The requested frame index this result is for.
    pub index: i64,
    /// The decoded frame, or the error for this index (e.g.
    /// [`Status::Cancelled`](crate::Status::Cancelled) for a cancelled frame).
    pub result: Result<Frame>,
}

/// Order in which [`decode_frames_stream`] yields frames.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum FrameOrder {
    /// Yield each frame as soon as it finishes decoding — maximum throughput,
    /// no head-of-line blocking. Frames may arrive in any order.
    #[default]
    Completion,
    /// Yield frames in the order of the requested indices. A worker that
    /// finishes ahead of its turn parks until that frame is emitted, so at most
    /// the decoder's worker count of frames sit decoded-but-unsent — the worker
    /// count bounds the reorder window, with no separate knob. The cost is that
    /// a slow early frame stalls delivery of the later ones already decoded.
    Indexed,
}

/// Options for [`decode_frames_stream`].
#[derive(Clone, Copy, Debug, Default)]
pub struct StreamOptions {
    /// How many decoded frames may sit buffered before the worker pool blocks
    /// (backpressure). `None` uses [`DEFAULT_CHANNEL_DEPTH`]; `Some(0)` is
    /// rejected.
    pub channel_depth: Option<usize>,
    /// Delivery order. Defaults to [`FrameOrder::Completion`].
    pub order: FrameOrder,
}

/// Serialises out-of-order completions back into requested-index order for
/// [`FrameOrder::Indexed`]. Workers call [`wait_turn`](OrderGate::wait_turn)
/// from arbitrary threads; the call parks until that index is next, then the
/// returned guard holds the gate (so the caller emits before any later frame)
/// and advances to the next slot when dropped.
///
/// Deadlock-free given the C scheduler claims indices in array order: the slot
/// the gate is waiting on is always already in flight, never starved behind
/// later frames that finished first.
struct OrderGate<'i> {
    indices: &'i [i64],
    next_pos: Mutex<usize>,
    turn: Condvar,
}

impl<'i> OrderGate<'i> {
    fn new(indices: &'i [i64]) -> OrderGate<'i> {
        OrderGate {
            indices,
            next_pos: Mutex::new(0),
            turn: Condvar::new(),
        }
    }

    /// Blocks until `index` is the next slot, returning a guard that advances
    /// the gate on drop. The caller must emit before dropping it.
    fn wait_turn(&self, index: i64) -> OrderSlot<'_, 'i> {
        let mut pos = self.next_pos.lock().unwrap();
        while self.indices[*pos] != index {
            pos = self.turn.wait(pos).unwrap();
        }
        OrderSlot { gate: self, pos }
    }
}

/// Guard from [`OrderGate::wait_turn`]; advances the gate to the next slot and
/// wakes the next waiter when dropped.
struct OrderSlot<'g, 'i> {
    gate: &'g OrderGate<'i>,
    pos: std::sync::MutexGuard<'g, usize>,
}

impl Drop for OrderSlot<'_, '_> {
    fn drop(&mut self) {
        *self.pos += 1;
        self.gate.turn.notify_all();
    }
}

/// An async stream of decoded frames produced by [`decode_frames_stream`].
///
/// Yields [`DecodedFrame`]s until the batch is exhausted, then `None`. Call
/// [`finish`](FrameStream::finish) afterwards for the overall decode status
/// (including any setup error that produced an empty stream).
#[derive(Debug)]
pub struct FrameStream {
    rx: mpsc::Receiver<DecodedFrame>,
    join: Option<JoinHandle<Result<()>>>,
    cancel: Arc<Cancel>,
}

/// Spawns a batch decode and returns an async [`FrameStream`] over its frames.
///
/// Ownership of `video` moves into a blocking worker; the decoder is built,
/// configured by `configure` (set options, NN model, dropout here), committed,
/// and run there. Must be called from within a Tokio runtime. See
/// [`StreamOptions`] for delivery order and buffering.
///
/// Cancel from elsewhere via [`FrameStream::cancel`] or
/// [`cancel_handle`](FrameStream::cancel_handle); the handle is also used
/// internally to stop the decode if the stream is dropped early.
///
/// ```no_run
/// use chromadec::{DecoderKind, Plane, StreamOptions, Video};
/// use futures_util::StreamExt;
///
/// # async fn run() -> chromadec::Result<()> {
/// let video = Video::open_composite("capture.tbc", None, None)?;
/// let mut frames = chromadec::decode_frames_stream(
///     video,
///     DecoderKind::Ntsc3d,
///     0..16,
///     StreamOptions::default(),
///     |dec| dec.set_option_f64(chromadec::options::CHROMA_GAIN, 1.0),
/// )?;
///
/// while let Some(decoded) = frames.next().await {
///     let frame = decoded.result?;
///     let y = frame.plane_u16(Plane::Y)?;
///     println!("frame {}: {}x{}", decoded.index, y.width(), y.height());
/// }
/// frames.finish().await
/// # }
/// ```
pub fn decode_frames_stream<I, C>(
    video: Video,
    kind: DecoderKind,
    frame_indices: I,
    options: StreamOptions,
    configure: C,
) -> Result<FrameStream>
where
    I: IntoIterator<Item = i64>,
    C: for<'v> FnOnce(&mut Decoder<'v>) -> Result<()> + Send + 'static,
{
    let channel_depth = options.channel_depth.unwrap_or(DEFAULT_CHANNEL_DEPTH);
    if channel_depth == 0 {
        return Err(Error::new(
            Status::InvalidArg,
            Some("channel_depth must be non-zero".to_owned()),
        ));
    }
    let order = options.order;
    let frame_indices: Vec<i64> = frame_indices.into_iter().collect();
    let cancel = Arc::new(Cancel::new()?);
    let (tx, rx) = mpsc::channel(channel_depth);

    let join = tokio::task::spawn_blocking({
        let cancel = Arc::clone(&cancel);
        move || -> Result<()> {
            let mut video = video;
            let mut decoder = Decoder::new(&mut video, kind)?;
            configure(&mut decoder)?;
            decoder.commit()?;

            // `blocking_send` on a full channel parks this C worker thread
            // (backpressure); a closed channel means the consumer is gone, so
            // request cancel to stop decoding.
            match order {
                FrameOrder::Completion => {
                    let on_frame = |index: i64, result: Result<Frame>| {
                        if tx.blocking_send(DecodedFrame { index, result }).is_err() {
                            cancel.request();
                        }
                    };
                    decoder.decode_frames(&frame_indices, Some(&cancel), on_frame)
                }
                FrameOrder::Indexed => {
                    // A worker that finishes ahead of its turn parks in the gate
                    // until its slot is next, then emits while still holding the
                    // gate so no later frame can overtake. Because a parked
                    // worker stops pulling new indices, at most one frame per
                    // worker sits decoded-but-unsent.
                    let gate = OrderGate::new(&frame_indices);
                    let on_frame = |index: i64, result: Result<Frame>| {
                        let _slot = gate.wait_turn(index);
                        if tx.blocking_send(DecodedFrame { index, result }).is_err() {
                            cancel.request();
                        }
                        // `_slot` drops here, advancing the gate to the next.
                    };
                    decoder.decode_frames(&frame_indices, Some(&cancel), on_frame)
                }
            }
        }
    });

    Ok(FrameStream {
        rx,
        join: Some(join),
        cancel,
    })
}

impl FrameStream {
    /// Receives the next decoded frame, or `None` once the batch is done.
    ///
    /// Equivalent to the [`Stream`](futures_core::Stream) impl; offered as an
    /// inherent method so the stream is usable without importing `StreamExt`.
    pub async fn recv(&mut self) -> Option<DecodedFrame> {
        self.rx.recv().await
    }

    /// Requests cancellation; queued frames will report
    /// [`Status::Cancelled`](crate::Status::Cancelled).
    pub fn cancel(&self) {
        self.cancel.request();
    }

    /// A shareable handle to this stream's cancellation, for requesting cancel
    /// from another task.
    pub fn cancel_handle(&self) -> Arc<Cancel> {
        Arc::clone(&self.cancel)
    }

    /// Awaits the worker and returns the overall decode status. Surfaces setup
    /// errors (decoder build/commit) that produced an empty stream. Idempotent
    /// once it has consumed the join handle.
    pub async fn finish(mut self) -> Result<()> {
        self.join_worker().await
    }

    async fn join_worker(&mut self) -> Result<()> {
        match self.join.take() {
            None => Ok(()),
            Some(join) => match join.await {
                Ok(result) => result,
                Err(err) if err.is_panic() => std::panic::resume_unwind(err.into_panic()),
                Err(_) => Err(Error::internal("decode worker task was aborted")),
            },
        }
    }
}

impl futures_core::Stream for FrameStream {
    type Item = DecodedFrame;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        self.rx.poll_recv(cx)
    }
}

impl Drop for FrameStream {
    fn drop(&mut self) {
        // Stop the worker promptly if the stream is abandoned mid-decode; the
        // detached task then drains and exits on its own.
        self.cancel.request();
    }
}

#[cfg(test)]
mod tests {
    use super::OrderGate;
    use std::sync::Mutex;
    use std::thread;
    use std::time::Duration;

    // Each "worker" calls wait_turn from its own thread, staggered so higher
    // positions arrive first — the gate must hold them until the lower ones
    // catch up. A correct gate yields exactly the requested index order; a
    // deadlock or ordering bug makes the test hang or the assert fail.
    fn assert_gate_orders(indices: Vec<i64>) {
        let n = indices.len();
        let gate = OrderGate::new(&indices);
        let out = Mutex::new(Vec::<i64>::new());
        thread::scope(|s| {
            for (i, &index) in indices.iter().enumerate() {
                let gate = &gate;
                let out = &out;
                s.spawn(move || {
                    thread::sleep(Duration::from_micros((n - i) as u64 * 50));
                    let _slot = gate.wait_turn(index);
                    out.lock().unwrap().push(index);
                    // `_slot` drops here, advancing the gate.
                });
            }
        });
        assert_eq!(out.into_inner().unwrap(), indices);
    }

    #[test]
    fn order_gate_serialises_reverse_arrival() {
        assert_gate_orders((0..64).collect());
    }

    #[test]
    fn order_gate_handles_noncontiguous_indices() {
        assert_gate_orders(vec![10, 25, 7, 99, 3, 41]);
    }

    #[test]
    fn order_gate_handles_duplicate_indices() {
        assert_gate_orders(vec![10, 20, 20, 5, 5, 5, 8]);
    }
}
