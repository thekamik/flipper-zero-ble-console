//! A single session: connect to the Flipper, run a shell in a pty and relay bytes.

use std::time::Duration;

use anyhow::{Context, Result, bail};
use bluer::{Adapter, Address, DeviceEvent, DeviceProperty};
use futures::{StreamExt, pin_mut};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tracing::{debug, info};

use crate::{
    ble::{self, RxWriter},
    config::Config,
    flow::Window,
    protocol,
    shell::{self, Shell},
};

/// Why a shell run ended.
enum ShellEnd {
    Exited,
    Disconnected,
}

pub async fn run(adapter: &Adapter, addr: Address, cfg: &Config) -> Result<()> {
    let dev = ble::find_device(adapter, addr, Duration::from_secs(10)).await?;
    if !dev.is_paired().await? {
        bail!("{addr} is not paired, pair it first (bluetoothctl or `piterm-server pair`)");
    }
    ble::connect(&dev).await?;
    info!("connected to {addr}");

    let chars = ble::find_chars(&dev).await?;
    let initial =
        protocol::parse_window(&chars.flow.read().await?).context("invalid flow control value")?;
    debug!("initial window: {initial}");
    let window = Window::new(initial);

    // Window notifications are handled by a separate task, so a send that is
    // waiting for the window can never block receiving the update it waits for.
    let flow_stream = chars.flow.notify().await?;
    let _flow_task = AbortOnDrop(tokio::spawn({
        let window = window.clone();
        async move {
            pin_mut!(flow_stream);
            while let Some(v) = flow_stream.next().await {
                if let Some(w) = protocol::parse_window(&v) {
                    debug!("window: {w}");
                    window.set(w);
                }
            }
        }
    }));

    let tx_stream = chars.tx.notify().await?;
    let dev_events = dev.events().await?;
    pin_mut!(tx_stream, dev_events);
    let rx = RxWriter::new(chars.rx).await;

    loop {
        let mut sh = shell::spawn(cfg)?;
        info!("started shell {}", cfg.shell);

        let end = {
            let sender = pump_output(&mut sh.reader, &rx, &window);
            pin_mut!(sender);
            loop {
                tokio::select! {
                    res = &mut sender => { res?; break ShellEnd::Exited; }
                    v = tx_stream.next() => match v {
                        Some(bytes) => sh.writer.write_all(&bytes).await?,
                        None => break ShellEnd::Disconnected,
                    },
                    evt = dev_events.next() => match evt {
                        Some(DeviceEvent::PropertyChanged(DeviceProperty::Connected(false))) | None => {
                            break ShellEnd::Disconnected;
                        }
                        _ => {}
                    },
                }
            }
        };

        close_shell(sh).await;
        match end {
            ShellEnd::Disconnected => {
                info!("Flipper disconnected");
                return Ok(());
            }
            ShellEnd::Exited => {
                info!("shell exited, starting a new one");
                send_windowed(&rx, &window, b"\r\n[piterm] new shell\r\n").await?;
            }
        }
    }
}

/// Reads shell output and sends it to RX. Returns when the shell exits.
async fn pump_output(
    reader: &mut pty_process::OwnedReadPty,
    rx: &RxWriter,
    window: &Window,
) -> Result<()> {
    let mut buf = [0u8; 1024];
    loop {
        let n = match reader.read(&mut buf).await {
            Ok(0) => return Ok(()),
            Ok(n) => n,
            Err(e) if shell::is_eof(&e) => return Ok(()),
            Err(e) => return Err(e.into()),
        };
        send_windowed(rx, window, &buf[..n]).await?;
    }
}

/// Sends `data` in chunks that fit both the MTU and the Flipper's free window.
async fn send_windowed(rx: &RxWriter, window: &Window, mut data: &[u8]) -> Result<()> {
    while !data.is_empty() {
        let n = window.acquire(rx.chunk().min(data.len())).await;
        rx.write(&data[..n]).await.context("RX write failed")?;
        data = &data[n..];
    }
    Ok(())
}

struct AbortOnDrop(tokio::task::JoinHandle<()>);

impl Drop for AbortOnDrop {
    fn drop(&mut self) {
        self.0.abort();
    }
}

async fn close_shell(mut sh: Shell) {
    let _ = sh.child.start_kill();
    let _ = sh.child.wait().await;
}
