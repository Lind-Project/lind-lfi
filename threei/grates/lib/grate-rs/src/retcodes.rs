//! Decoding of the results of the two 3i copy calls.
//!
//! Kept free of any FFI so it can be unit-tested on the host
//! (`rustc --edition 2021 --test src/retcodes.rs && ./retcodes`).
//!
//! `copy_data_between_cages` returns the destination address on success. That is a 64-bit
//! sandbox pointer, but anything that passes through a C `int` (a handler's return, the old
//! wrapper) keeps only its low 32 bits, and a valid offset may have the high bit set. A failure
//! is one of: the runtime's abort code `0xE0010001` (for every error, as in Lind), the "no such
//! cage" code `0xE0010002`, or an ordinary `-errno` from a handler that denied the call.
//! `copy_handler_table_to_cage` returns 0 on success and any of those on failure.

pub const ELINDAPIABORTED: u64 = 0xE001_0001;
pub const ELINDESRCH: u64 = 0xE001_0002;

/// True if `raw` is one of the two 3i failure codes, whether it arrived full width, truncated to
/// 32 bits, or sign-extended from a C `int`.
fn is_3i_code(raw: i64) -> bool {
    let hi = raw >> 32;
    let lo = raw as u32 as u64;
    (hi == 0 || hi == -1) && (lo == ELINDAPIABORTED || lo == ELINDESRCH)
}

/// A small negative value is `-errno` from a handler that refused the call. Real destination
/// offsets in the last page of a sandbox are never valid, so this range cannot hide a success.
fn is_neg_errno(raw: i64) -> bool {
    (-4095..=-1).contains(&raw)
}

/// Decode the result of `copy_data_between_cages`.
pub fn decode_copy_data(raw: i64) -> Result<(), i32> {
    if is_3i_code(raw) {
        Err(raw as u32 as i32)
    } else if is_neg_errno(raw) {
        Err(raw as i32)
    } else {
        Ok(())
    }
}

/// Decode the result of `copy_handler_table_to_cage`: success is exactly zero.
pub fn decode_copy_htab(raw: i64) -> Result<(), i32> {
    if raw == 0 {
        Ok(())
    } else {
        Err(raw as i32)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn data_copy_failures() {
        assert!(decode_copy_data(0xE001_0001).is_err()); // full-width abort code
        assert!(decode_copy_data(0xE001_0001u32 as i32 as i64).is_err()); // sign-extended int
        assert!(decode_copy_data(0xE001_0002).is_err()); // no such cage
        assert!(decode_copy_data(-1).is_err()); // handler denied: -EPERM-like
        assert!(decode_copy_data(-1).unwrap_err() == -1);
        assert!(decode_copy_data(-13).is_err());
    }

    #[test]
    fn data_copy_successes() {
        assert!(decode_copy_data(0x10_0000_1000).is_ok()); // a normal sandbox pointer
        assert!(decode_copy_data(0x1000).is_ok());
        // a valid destination offset with the high bit set, truncated through an int
        assert!(decode_copy_data(0x8000_1000u32 as i32 as i64).is_ok());
        assert!(decode_copy_data(0xF000_0000u32 as i32 as i64).is_ok());
        // an address that merely shares the low 32 bits with the code is a different value
        assert!(decode_copy_data(0x10_E001_0001).is_ok());
        assert!(decode_copy_data(0).is_ok()); // zero-length copies report their address
    }

    #[test]
    fn table_copy() {
        assert!(decode_copy_htab(0).is_ok());
        assert_eq!(decode_copy_htab(-1), Err(-1)); // -EPERM from the runtime
        assert!(decode_copy_htab(-3).is_err());
        assert!(decode_copy_htab(0xE001_0002).is_err()); // no such cage
        assert!(decode_copy_htab(0xE001_0001).is_err());
        assert!(decode_copy_htab(0xE001_0001u32 as i32 as i64).is_err());
    }
}
