//! Shell running in a pseudo-terminal.

use anyhow::{Context, Result};
use pty_process::{OwnedReadPty, OwnedWritePty, Size};
use tokio::process::Child;

use crate::config::Config;

pub struct Shell {
    pub reader: OwnedReadPty,
    pub writer: OwnedWritePty,
    pub child: Child,
}

pub fn spawn(cfg: &Config) -> Result<Shell> {
    let (pty, pts) = pty_process::open().context("cannot open pty")?;
    pty.resize(Size::new(cfg.term_rows, cfg.term_cols))
        .context("cannot set pty size")?;

    let mut cmd = pty_process::Command::new(&cfg.shell)
        .args(&cfg.shell_args)
        .env("TERM", &cfg.term)
        .kill_on_drop(true);
    if let Some(home) = std::env::var_os("HOME") {
        cmd = cmd.current_dir(home);
    }
    let child = cmd
        .spawn(pts)
        .with_context(|| format!("cannot start {}", cfg.shell))?;

    let (reader, writer) = pty.into_split();
    Ok(Shell {
        reader,
        writer,
        child,
    })
}

/// On Linux, reading the pty after the shell exits fails with EIO instead of EOF.
pub fn is_eof(err: &std::io::Error) -> bool {
    const EIO: i32 = 5;
    err.raw_os_error() == Some(EIO)
}
