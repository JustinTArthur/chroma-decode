// SPDX-License-Identifier: GPL-3.0-or-later

use std::ffi::CString;
use std::path::Path;
use std::ptr::NonNull;

use crate::error::{Error, Result, Status};

pub(crate) fn path_to_cstring(path: &Path) -> Result<CString> {
    #[cfg(unix)]
    let bytes = std::os::unix::ffi::OsStrExt::as_bytes(path.as_os_str()).to_vec();
    #[cfg(not(unix))]
    let bytes = path
        .to_str()
        .ok_or_else(|| invalid_path(path))?
        .as_bytes()
        .to_vec();
    CString::new(bytes).map_err(|_| invalid_path(path))
}

fn invalid_path(path: &Path) -> Error {
    Error::new(
        Status::InvalidArg,
        Some(format!(
            "path is not representable as a C string: {}",
            path.display()
        )),
    )
}

pub(crate) fn non_null<T>(raw: *mut T) -> Result<NonNull<T>> {
    NonNull::new(raw).ok_or_else(|| Error::internal("library returned CHD_OK with a null handle"))
}
