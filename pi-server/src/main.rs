mod ble;
mod config;
mod flow;
mod protocol;
mod session;
mod shell;

use std::{path::PathBuf, time::Duration};

use anyhow::{Result, bail};
use bluer::Address;
use clap::{Parser, Subcommand};
use tracing::{info, warn};

use crate::config::Config;

/// Linux shell on the Flipper Zero screen, over Bluetooth LE.
#[derive(Parser)]
#[command(version)]
struct Cli {
    /// Configuration file (defaults are used if it does not exist).
    #[arg(long, short, default_value = config::DEFAULT_PATH)]
    config: PathBuf,
    #[command(subcommand)]
    command: Option<Command>,
}

#[derive(Subcommand)]
enum Command {
    /// Daemon: connect to the Flipper and run a shell (default).
    Run,
    /// List nearby Flippers running the PiTerm app.
    Scan {
        #[arg(long, default_value_t = 10)]
        secs: u64,
    },
    /// Connect to the Flipper and print its services and characteristics.
    Inspect { addr: Option<Address> },
    /// Pair with the Flipper (type the PIN shown on the Flipper here).
    Pair {
        addr: Option<Address>,
        /// Store the address as `flipper_addr` in the configuration file.
        #[arg(long)]
        save: bool,
    },
}

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "info".into()),
        )
        .init();

    let cli = Cli::parse();
    let cfg = if cli.config.exists() {
        Config::load(&cli.config)?
    } else {
        Config::default()
    };
    let session = bluer::Session::new().await?;
    let adapter = ble::open_adapter(&session, &cfg).await?;

    match cli.command.unwrap_or(Command::Run) {
        Command::Run => run(&adapter, &cfg).await,
        Command::Scan { secs } => ble::scan(&adapter, secs).await,
        Command::Inspect { addr } => {
            let addr = match addr {
                Some(a) => a,
                None => resolve_addr(&adapter, &cfg).await?,
            };
            ble::inspect(&adapter, addr).await
        }
        Command::Pair { addr, save } => {
            let addr = match addr.or(cfg.flipper_addr()?) {
                Some(a) => a,
                None => {
                    bail!(
                        "missing MAC: `piterm-server pair <MAC>` (find it with `piterm-server scan`)"
                    )
                }
            };
            ble::pair(&session, &adapter, addr).await?;
            if save {
                config::save_flipper_addr(&cli.config, addr)?;
                println!("Saved {addr} to {}", cli.config.display());
            }
            Ok(())
        }
    }
}

/// `flipper_addr` from the configuration, or the only paired PiTerm Flipper.
async fn resolve_addr(adapter: &bluer::Adapter, cfg: &Config) -> Result<Address> {
    match cfg.flipper_addr()? {
        Some(addr) => Ok(addr),
        None => ble::find_paired_piterm(adapter).await,
    }
}

/// Daemon loop: connect → session → wait → repeat. The address is resolved on
/// every iteration, so the service can wait until the Flipper gets paired.
async fn run(adapter: &bluer::Adapter, cfg: &Config) -> Result<()> {
    cfg.flipper_addr()?; // fail fast on a malformed MAC
    let delay = Duration::from_secs(cfg.reconnect_delay_secs);
    info!("PiTerm: adapter {}", adapter.name());
    loop {
        match resolve_addr(adapter, cfg).await {
            Ok(addr) => {
                if let Err(e) = session::run(adapter, addr, cfg).await {
                    warn!("session {addr}: {e:#}");
                }
            }
            Err(e) => warn!("{e:#}"),
        }
        tokio::time::sleep(delay).await;
    }
}
