//! Whether this system runs the launcher's programs as what they are built for: the launcher and
//! the game are x64 programs, which a system of another processor runs only by emulating them.

#[cfg(windows)]
use crate::platform;

/// The machine a program's header names for x64, IMAGE_FILE_MACHINE_AMD64: the only native
/// machine the launcher and the game run on as built.
#[cfg(any(windows, test))]
const X64_MACHINE: u16 = 0x8664;

/// A system that runs x64 programs only by emulating them, such as Windows on ARM64.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Unsupported {
    /// The system's own machine, as a program's header names one: 0xAA64 for ARM64.
    pub native_machine: u16,
}

/// Whether this system runs the launcher's x64 programs natively. Windows is asked what machine
/// it is, and one without the call that says is taken to emulate nothing, since every Windows
/// that emulates x64 has it; a build for another system always runs on the processor it was built
/// for.
pub fn host_supported() -> Result<(), Unsupported> {
    #[cfg(windows)]
    {
        judge_machine(platform::native_machine())
    }
    #[cfg(not(windows))]
    {
        Ok(())
    }
}

/// Judges a system by the native machine it named, `None` when it named none.
#[cfg(any(windows, test))]
pub(crate) fn judge_machine(native: Option<u16>) -> Result<(), Unsupported> {
    match native {
        None | Some(X64_MACHINE) => Ok(()),
        Some(native_machine) => Err(Unsupported { native_machine }),
    }
}

#[cfg(test)]
mod tests {
    use super::{Unsupported, X64_MACHINE, host_supported, judge_machine};

    /// The machines a program's header names for ARM64, x86 and ARM (IMAGE_FILE_MACHINE_ARM64,
    /// I386 and ARMNT), and the unknown machine.
    const ARM64_MACHINE: u16 = 0xAA64;
    const X86_MACHINE: u16 = 0x014C;
    const ARM_MACHINE: u16 = 0x01C4;
    const UNKNOWN_MACHINE: u16 = 0;

    #[test]
    fn only_a_native_x64_machine_is_supported() {
        assert_eq!(judge_machine(Some(X64_MACHINE)), Ok(()));
        // A system that names no machine lacks the call, and with it any emulation.
        assert_eq!(judge_machine(None), Ok(()));
        for native_machine in [ARM64_MACHINE, X86_MACHINE, ARM_MACHINE, UNKNOWN_MACHINE] {
            assert_eq!(
                judge_machine(Some(native_machine)),
                Err(Unsupported { native_machine }),
                "{native_machine:#06x}"
            );
        }
        // The system the tests run on is the one they are built for, so it judges as it names.
        #[cfg(not(windows))]
        assert_eq!(host_supported(), Ok(()));
        #[cfg(windows)]
        assert_eq!(
            host_supported(),
            judge_machine(crate::platform::native_machine())
        );
    }

    #[cfg(windows)]
    #[test]
    fn the_machines_are_the_systems() {
        use windows_sys::Win32::System::SystemInformation as system;

        assert_eq!(X64_MACHINE, system::IMAGE_FILE_MACHINE_AMD64);
        assert_eq!(ARM64_MACHINE, system::IMAGE_FILE_MACHINE_ARM64);
        assert_eq!(X86_MACHINE, system::IMAGE_FILE_MACHINE_I386);
        assert_eq!(ARM_MACHINE, system::IMAGE_FILE_MACHINE_ARMNT);
        assert_eq!(UNKNOWN_MACHINE, system::IMAGE_FILE_MACHINE_UNKNOWN);
        // Every Windows the tests run on has IsWow64Process2, and names a machine through it.
        assert!(crate::platform::native_machine().is_some());
    }
}
