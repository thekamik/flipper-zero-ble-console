//! BlueZ: adapter, Flipper discovery, GATT characteristics and pairing.

use std::time::Duration;

use anyhow::{Context, Result, bail};
use bluer::{
    Adapter, AdapterEvent, Address, Device, Session,
    agent::{Agent, AgentHandle, ReqError, RequestPasskey},
    gatt::{
        CharacteristicWriter,
        remote::{Characteristic, Service},
    },
};
use futures::{StreamExt, pin_mut};
use tokio::io::{AsyncBufReadExt, BufReader};
use tracing::{debug, info, warn};

use crate::{config::Config, protocol};

pub async fn open_adapter(session: &Session, cfg: &Config) -> Result<Adapter> {
    let adapter = match &cfg.adapter {
        Some(name) => session.adapter(name)?,
        None => session.default_adapter().await?,
    };
    adapter
        .set_powered(true)
        .await
        .with_context(|| format!("cannot power on adapter {}", adapter.name()))?;
    Ok(adapter)
}

/// Makes sure BlueZ knows the device. Paired devices are always known,
/// new ones have to show up in a scan first.
pub async fn find_device(adapter: &Adapter, addr: Address, timeout: Duration) -> Result<Device> {
    if adapter.device_addresses().await?.contains(&addr) {
        return Ok(adapter.device(addr)?);
    }
    info!("looking for {addr} (the PiTerm app must be running on the Flipper)");
    let events = adapter.discover_devices().await?;
    pin_mut!(events);
    tokio::time::timeout(timeout, async {
        while let Some(evt) = events.next().await {
            if let AdapterEvent::DeviceAdded(a) = evt
                && a == addr
            {
                return Ok(adapter.device(addr)?);
            }
        }
        bail!("discovery stopped")
    })
    .await
    .with_context(|| format!("{addr} not found within {}s", timeout.as_secs()))?
}

/// The only paired device named "PiTerm ...". Used when the configuration has
/// no `flipper_addr`, so pairing with the usual system tools is enough.
pub async fn find_paired_piterm(adapter: &Adapter) -> Result<Address> {
    let mut found = Vec::new();
    for addr in adapter.device_addresses().await? {
        let dev = adapter.device(addr)?;
        let name = dev.name().await?.unwrap_or_default();
        if name.starts_with(protocol::ADV_NAME_PREFIX) && dev.is_paired().await? {
            found.push((addr, name));
        }
    }
    match found.as_slice() {
        [] => bail!(
            "no paired \"PiTerm ...\" device: start PiTerm on the Flipper and pair with it \
             (bluetoothctl or `piterm-server pair`)"
        ),
        [(addr, name)] => {
            info!("using paired device {addr} ({name})");
            Ok(*addr)
        }
        _ => {
            let list: Vec<_> = found.iter().map(|(a, n)| format!("{a} ({n})")).collect();
            bail!(
                "several paired PiTerm devices: {}; choose one with `flipper_addr`",
                list.join(", ")
            )
        }
    }
}

pub async fn connect(dev: &Device) -> Result<()> {
    if !dev.is_connected().await? {
        dev.connect().await.context("connection failed")?;
    }
    Ok(())
}

pub struct Chars {
    pub tx: Characteristic,
    pub rx: Characteristic,
    pub flow: Characteristic,
}

async fn find_service(dev: &Device) -> Result<Service> {
    for svc in dev.services().await? {
        if svc.uuid().await? == protocol::SERVICE {
            return Ok(svc);
        }
    }
    bail!("device has no PiTerm service (is the PiTerm app running on the Flipper?)")
}

pub async fn find_chars(dev: &Device) -> Result<Chars> {
    let svc = find_service(dev).await?;
    let (mut tx, mut rx, mut flow) = (None, None, None);
    for ch in svc.characteristics().await? {
        match ch.uuid().await? {
            u if u == protocol::TX => tx = Some(ch),
            u if u == protocol::RX => rx = Some(ch),
            u if u == protocol::FLOW => flow = Some(ch),
            _ => {}
        }
    }
    Ok(Chars {
        tx: tx.context("TX characteristic missing")?,
        rx: rx.context("RX characteristic missing")?,
        flow: flow.context("flow control characteristic missing")?,
    })
}

/// Writes to RX. Prefers `AcquireWrite` (write without response with a known
/// MTU) and falls back to plain 20-byte writes when BlueZ does not offer it.
pub enum RxWriter {
    Io(CharacteristicWriter),
    Plain(Characteristic),
}

impl RxWriter {
    pub async fn new(rx: Characteristic) -> Self {
        match rx.write_io().await {
            Ok(w) => {
                debug!("RX via AcquireWrite, MTU {}", w.mtu());
                Self::Io(w)
            }
            Err(e) => {
                warn!("AcquireWrite unavailable ({e}), using plain writes");
                Self::Plain(rx)
            }
        }
    }

    /// Maximum size of a single write.
    pub fn chunk(&self) -> usize {
        match self {
            Self::Io(w) => w.mtu().min(protocol::RX_VALUE_LEN_MAX),
            Self::Plain(_) => protocol::DEFAULT_CHUNK,
        }
    }

    pub async fn write(&self, data: &[u8]) -> Result<()> {
        match self {
            Self::Io(w) => w.send(data).await?,
            Self::Plain(ch) => ch.write(data).await?,
        }
        Ok(())
    }
}

/// `piterm-server scan`: lists advertising Flippers running PiTerm.
pub async fn scan(adapter: &Adapter, secs: u64) -> Result<()> {
    println!("Scanning for {secs}s on {}...", adapter.name());
    let events = adapter.discover_devices().await?;
    pin_mut!(events);
    let _ = tokio::time::timeout(Duration::from_secs(secs), async {
        while let Some(evt) = events.next().await {
            let AdapterEvent::DeviceAdded(addr) = evt else {
                continue;
            };
            let Ok(dev) = adapter.device(addr) else {
                continue;
            };
            let name = dev.name().await.ok().flatten().unwrap_or_default();
            if name.starts_with(protocol::ADV_NAME_PREFIX) {
                let rssi = dev.rssi().await.ok().flatten().unwrap_or_default();
                let paired = dev.is_paired().await.unwrap_or(false);
                println!("{addr}  {name:<20} RSSI {rssi:>4}  paired: {paired}");
            }
        }
    })
    .await;
    Ok(())
}

/// `piterm-server inspect`: connects and prints services and characteristics.
pub async fn inspect(adapter: &Adapter, addr: Address) -> Result<()> {
    let dev = find_device(adapter, addr, Duration::from_secs(15)).await?;
    connect(&dev).await?;
    println!("{addr}  {}", dev.name().await?.unwrap_or_default());
    for svc in dev.services().await? {
        println!("service {}", svc.uuid().await?);
        for ch in svc.characteristics().await? {
            println!("  char {}  {:?}", ch.uuid().await?, ch.flags().await?);
        }
    }
    Ok(())
}

/// BlueZ agent that asks for the PIN shown on the Flipper (on stdin).
async fn register_pin_agent(session: &Session) -> Result<AgentHandle> {
    let agent = Agent {
        request_default: true,
        request_passkey: Some(Box::new(|req: RequestPasskey| {
            Box::pin(async move {
                println!("Enter the PIN shown on the Flipper ({}):", req.device);
                let mut line = String::new();
                BufReader::new(tokio::io::stdin())
                    .read_line(&mut line)
                    .await
                    .map_err(|_| ReqError::Canceled)?;
                line.trim().parse::<u32>().map_err(|_| ReqError::Rejected)
            })
        })),
        ..Default::default()
    };
    Ok(session.register_agent(agent).await?)
}

/// `piterm-server pair`: pairs with the Flipper and marks it as trusted.
pub async fn pair(session: &Session, adapter: &Adapter, addr: Address) -> Result<()> {
    let _agent = register_pin_agent(session).await?;
    let dev = find_device(adapter, addr, Duration::from_secs(30)).await?;
    if dev.is_paired().await? {
        println!("{addr} is already paired.");
    } else {
        println!("Pairing with {addr}...");
        dev.pair().await.context("pairing failed")?;
        println!("Paired.");
    }
    dev.set_trusted(true).await?;
    // Disconnect so the daemon can open its own connection.
    let _ = dev.disconnect().await;
    Ok(())
}
