//! Internal libc/Lind FFI and ABI shim layer.
//!
//! This module contains:
//! - constants used for low-level memory/process primitives
//! - target-specific ABI type shims (for Lind compatibility)
//! - raw `extern "C"` bindings used by the safe APIs in `lib.rs`
use core::ffi::{c_char, c_int};
use libc::{pid_t, size_t};
use std::ffi::{c_uint, c_void};
use std::io::Write;

#[allow(non_camel_case_types)]
// LFI guests are x86-64 musl: off_t is 64-bit.
pub type off_t = i64;

#[allow(non_camel_case_types)]
// musl's sem_t is 32 bytes (int[8]) on x86-64.
#[repr(C)]
pub struct sem_t {
    __size: [c_char; 32],
}

// `struct stat` with the Linux x86-64 (musl) memory layout, but the field types
// Lind's version uses so grate code written against it compiles unchanged: the
// fields Lind keeps 32-bit are followed by a padding field that makes up the
// 64-bit slot the kernel layout has (st_nlink, st_blksize and st_blocks are
// 64-bit there).
#[repr(C)]
#[derive(Eq, PartialEq, Default, Debug)]
pub struct stat {
    pub st_dev: u64,
    pub st_ino: u64,
    pub st_nlink: u32,
    pub __nlink_hi: u32,
    pub st_mode: u32,
    pub st_uid: u32,
    pub st_gid: u32,
    pub __pad0: u32,
    pub st_rdev: u64,
    pub st_size: u64,
    pub st_blksize: i32,
    pub __blksize_hi: i32,
    pub st_blocks: u32,
    pub __blocks_hi: u32,
    pub st_atim: [u64; 2],
    pub st_mtim: [u64; 2],
    pub st_ctim: [u64; 2],
    pub __unused: [i64; 3],
}

// Lind-compatible iovec struct for x86_64 guests.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug)]
pub struct iovec {
    pub iov_base: u64,
    pub iov_len: u64,
}

/// Flush stdio streams and terminate the process.
///
/// This helper is shared by both the public library logic (`lib.rs`) and
/// internal FFI error paths (`call_sys!`).
pub(crate) fn clean_exit(status: i32) -> ! {
    std::io::stdout().flush().unwrap();
    std::io::stderr().flush().unwrap();

    std::process::exit(status);
}

// External function bindings. `link_name` is used where Rust symbol names differ
// from Lind sysroot symbols.
unsafe extern "C" {
    // Lind threei-specific functions.
    #[link_name = "register_handler"]
    pub(crate) fn register_handler_impl(
        cageid: u64,
        syscall_nr: u64,
        grateid: u64,
        fn_ptr_addr: u64,
    ) -> c_int;

    #[link_name = "lfi3i_copy_data_long"]
    pub(crate) fn cp_data_long_impl(
        srcaddr: u64,
        srccage: u64,
        destaddr: u64,
        destcage: u64,
        len: u64,
        copytype: u64,
    ) -> i64;

    #[link_name = "lfi3i_copy_htab_long"]
    pub(crate) fn cp_htab_long_impl(srccage: u64, targetcage: u64) -> i64;

    #[link_name = "make_threei_call"]
    pub(crate) fn make_syscall_impl(
        callnumber: c_uint,
        callname: u64,
        self_cageid: u64,
        target_cageid: u64,
        arg1: u64,
        arg1cageid: u64,
        arg2: u64,
        arg2cageid: u64,
        arg3: u64,
        arg3cageid: u64,
        arg4: u64,
        arg4cageid: u64,
        arg5: u64,
        arg5cageid: u64,
        arg6: u64,
        arg6cageid: u64,
        translate_errno: c_int,
    ) -> c_int;

    // Helper to get the current cage ID.
    #[link_name = "getpid"]
    pub(crate) fn getpid_impl() -> pid_t;

    // Multiprocessing.
    pub fn fork() -> pid_t;
    pub fn execv(prog: *const c_char, argv: *const *const c_char) -> c_int;
    pub fn waitpid(pid: pid_t, status: *mut c_int, options: c_int) -> pid_t;

    // Memory management.
    pub fn mmap(
        addr: *mut c_void,
        len: size_t,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: off_t,
    ) -> *mut c_void;
    pub fn munmap(addr: *mut c_void, len: size_t) -> c_int;

    // POSIX semaphores.
    pub fn sem_init(sem: *mut sem_t, pshared: c_int, value: c_uint) -> c_int;
    pub fn sem_destroy(sem: *mut sem_t) -> c_int;
    pub fn sem_post(sem: *mut sem_t) -> c_int;
    pub fn sem_wait(sem: *mut sem_t) -> c_int;

    // Stat
    pub fn stat(path: *const c_char, buf: *mut stat) -> c_int;
}
