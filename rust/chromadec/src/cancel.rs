// SPDX-License-Identifier: GPL-3.0-or-later

use std::ptr::{self, NonNull};

use chromadec_sys as sys;

use crate::error::{Result, check};
use crate::util::non_null;

/// Cancellation handle for async decode (`chd_cancel_t`). Request from any
/// thread; in-flight frames complete, queued ones report `Cancelled`.
#[derive(Debug)]
pub struct Cancel {
    raw: NonNull<sys::chd_cancel>,
}

unsafe impl Send for Cancel {}
unsafe impl Sync for Cancel {}

impl Cancel {
    pub fn new() -> Result<Cancel> {
        let mut raw = ptr::null_mut();
        unsafe { check(sys::chd_cancel_create(&mut raw))? };
        Ok(Cancel {
            raw: non_null(raw)?,
        })
    }

    pub fn request(&self) {
        unsafe { sys::chd_cancel_request(self.raw.as_ptr()) };
    }

    pub fn is_requested(&self) -> bool {
        unsafe { sys::chd_cancel_is_requested(self.raw.as_ptr()) != 0 }
    }

    pub(crate) fn as_ptr(&self) -> *mut sys::chd_cancel {
        self.raw.as_ptr()
    }
}

impl Drop for Cancel {
    fn drop(&mut self) {
        unsafe { sys::chd_cancel_free(self.raw.as_ptr()) };
    }
}
