use std::path::Path;

use anyhow::{Context, Result};
use bluer::Address;
use serde::Deserialize;

pub const DEFAULT_PATH: &str = "/etc/piterm/config.toml";

#[derive(Debug, Clone, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Config {
    /// MAC of the Flipper in the PiTerm profile; the server connects only to it.
    /// Unset = the only paired device named "PiTerm ...".
    pub flipper_addr: Option<String>,
    /// BlueZ adapter name (e.g. "hci0"). Unset = default adapter.
    pub adapter: Option<String>,
    pub shell: String,
    pub shell_args: Vec<String>,
    pub term: String,
    /// Terminal size reported to the shell (the Flipper screen fits about 21x7 characters).
    pub term_cols: u16,
    pub term_rows: u16,
    pub reconnect_delay_secs: u64,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            flipper_addr: None,
            adapter: None,
            shell: "/bin/bash".into(),
            shell_args: vec!["-l".into()],
            term: "dumb".into(),
            term_cols: 21,
            term_rows: 7,
            reconnect_delay_secs: 5,
        }
    }
}

impl Config {
    pub fn load(path: &Path) -> Result<Self> {
        let text = std::fs::read_to_string(path)
            .with_context(|| format!("cannot read {}", path.display()))?;
        toml::from_str(&text).with_context(|| format!("invalid config {}", path.display()))
    }

    pub fn flipper_addr(&self) -> Result<Option<Address>> {
        self.flipper_addr
            .as_deref()
            .map(|s| {
                s.parse()
                    .with_context(|| format!("invalid MAC address: {s}"))
            })
            .transpose()
    }
}

/// Sets `flipper_addr` in the configuration file, keeping the rest of the file.
pub fn save_flipper_addr(path: &Path, addr: Address) -> Result<()> {
    let line = format!("flipper_addr = \"{addr}\"");
    let text = std::fs::read_to_string(path).unwrap_or_default();
    let mut replaced = false;
    let mut lines: Vec<String> = text
        .lines()
        .map(|l| {
            if l.trim_start().starts_with("flipper_addr") {
                replaced = true;
                line.clone()
            } else {
                l.to_string()
            }
        })
        .collect();
    if !replaced {
        lines.insert(0, line);
    }
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    std::fs::write(path, lines.join("\n") + "\n")
        .with_context(|| format!("cannot write {}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_example_config() {
        let cfg: Config = toml::from_str(include_str!("../config.example.toml")).unwrap();
        assert_eq!(cfg.shell, "/bin/bash");
        assert_eq!(cfg.flipper_addr().unwrap(), None);
    }

    #[test]
    fn defaults_apply() {
        let cfg: Config = toml::from_str("").unwrap();
        assert_eq!(cfg.term, "dumb");
        assert_eq!(cfg.flipper_addr().unwrap(), None);
    }

    #[test]
    fn parses_flipper_addr() {
        let cfg: Config = toml::from_str("flipper_addr = \"80:E1:27:F2:36:CE\"").unwrap();
        assert_eq!(
            cfg.flipper_addr().unwrap().unwrap().to_string(),
            "80:E1:27:F2:36:CE"
        );
        let bad: Config = toml::from_str("flipper_addr = \"xx\"").unwrap();
        assert!(bad.flipper_addr().is_err());
    }
}
