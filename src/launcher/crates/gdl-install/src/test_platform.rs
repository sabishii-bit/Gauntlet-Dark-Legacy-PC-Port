//! The platforms the tests install runtimes for and read and judge installations of: the one this
//! test program was built for, and the other one a release is built for.

use gdl_release::Platform;

/// The platform this test program was built for.
pub(crate) fn this_platform() -> Platform {
    Platform::of_this_build().expect("a release is built for the system the tests run on")
}

/// The platform a release is built for that this test program was not built for.
pub(crate) fn other_platform() -> Platform {
    let this = this_platform();
    Platform::ALL
        .into_iter()
        .find(|&platform| platform != this)
        .expect("releases are built for two platforms")
}
