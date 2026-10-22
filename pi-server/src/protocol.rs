//! PiTerm protocol constants. Single source of truth: `docs/PROTOCOL.md`.
//! Any change here requires the same change in `flipper-app/protocol.h`.

use bluer::Uuid;

/// Flipper serial service.
pub const SERVICE: Uuid = Uuid::from_u128(0x8fe5b3d5_2e7f_4a98_2a48_7acc60fe0000);
/// TX: Flipper -> Pi (indicate), bytes typed by the user.
pub const TX: Uuid = Uuid::from_u128(0x19ed82ae_ed21_4c9d_4145_228e61fe0000);
/// RX: Pi -> Flipper (write / write without response), shell output.
pub const RX: Uuid = Uuid::from_u128(0x19ed82ae_ed21_4c9d_4145_228e62fe0000);
/// Flow control: free space in the Flipper receive buffer, big-endian `u32` (read, notify).
pub const FLOW: Uuid = Uuid::from_u128(0x19ed82ae_ed21_4c9d_4145_228e63fe0000);
/// RPC status. The server NEVER writes it (writing 0 resets the Flipper BLE stack).
#[allow(dead_code)]
pub const RPC_STATUS: Uuid = Uuid::from_u128(0x19ed82ae_ed21_4c9d_4145_228e64fe0000);

/// Prefix of the name in the Flipper advertisement.
pub const ADV_NAME_PREFIX: &str = "PiTerm";

/// Safe write size when the MTU is unknown (ATT MTU 23 - 3).
pub const DEFAULT_CHUNK: usize = 20;
/// Maximum RX value length on the Flipper side.
pub const RX_VALUE_LEN_MAX: usize = 243;

/// Decodes the flow control characteristic value.
pub fn parse_window(value: &[u8]) -> Option<u32> {
    Some(u32::from_be_bytes(value.get(..4)?.try_into().ok()?))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn window_is_big_endian() {
        assert_eq!(parse_window(&[0, 0, 4, 0]), Some(1024));
        assert_eq!(parse_window(&[0, 0, 4]), None);
    }
}
