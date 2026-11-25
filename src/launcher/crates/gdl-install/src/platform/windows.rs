//! The Windows calls: setting resources into a program with BeginUpdateResourceW, UpdateResourceW
//! and EndUpdateResourceW, reading one back as data and mapping one as an image with
//! LoadLibraryExW for the tests, telling the shell with SHChangeNotify that a file it shows has
//! changed, asking with SetThreadExecutionState that the system stay awake, asking
//! IsWow64Process2 what machine the system is, asking with SetErrorMode that the system fail a
//! call rather than show a dialog, and what a folder chosen to install into is to the system: its
//! build from RtlGetVersion, the user's known folders from SHGetKnownFolderPath, an entry's
//! reparse tag from GetFileInformationByHandleEx, and the folders of the OneDrive accounts signed
//! in from the registry, and a path's names in full, short aliases replaced, from
//! GetLongPathNameW; whether an open file is the one at a name, by the identities
//! GetFileInformationByHandleEx and GetFileInformationByHandle give files; the proxy the user's
//! Internet Settings name, from the registry; and another process, opened by its id with
//! OpenProcess, to be waited for with WaitForSingleObject, asked when it started with
//! GetProcessTimes and which file it runs with QueryFullProcessImageNameW, and ended with
//! TerminateProcess; and whether this process has a console, from GetConsoleWindow. For the tests,
//! a program started under a filtered copy of this process's token, as a standard user's is, with
//! CreateRestrictedToken, SetTokenInformation and CreateProcessAsUserW, on a window station and
//! desktop made for it with CreateWindowStationW and CreateDesktopW, and what this process's own
//! token holds, from CheckTokenMembership and GetTokenInformation.

use std::ffi::OsString;
use std::fs::{File, OpenOptions};
use std::io;
use std::marker::PhantomData;
use std::mem::ManuallyDrop;
use std::os::windows::ffi::{OsStrExt, OsStringExt};
use std::os::windows::fs::OpenOptionsExt;
use std::os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle};
use std::path::{Path, PathBuf};
use std::ptr;
use std::time::Duration;

use gdl_files::{VerbatimPath, strip_verbatim};
use windows_sys::Wdk::System::SystemServices::RtlGetVersion;
use windows_sys::Win32::Foundation::{
    ERROR_ACCESS_DENIED, ERROR_INVALID_PARAMETER, ERROR_NO_MORE_ITEMS, ERROR_SUCCESS, FALSE,
    FILETIME, HANDLE, STATUS_SUCCESS, TRUE, WAIT_OBJECT_0, WAIT_TIMEOUT,
};
use windows_sys::Win32::Storage::FileSystem::{
    BY_HANDLE_FILE_INFORMATION, FILE_ATTRIBUTE_REPARSE_POINT, FILE_ATTRIBUTE_TAG_INFO,
    FILE_FLAG_BACKUP_SEMANTICS, FILE_FLAG_OPEN_REPARSE_POINT, FILE_ID_INFO, FileAttributeTagInfo,
    FileIdInfo, GetFileInformationByHandle, GetFileInformationByHandleEx, GetLongPathNameW,
};
use windows_sys::Win32::System::Com::CoTaskMemFree;
use windows_sys::Win32::System::Console::GetConsoleWindow;
use windows_sys::Win32::System::Diagnostics::Debug::{
    GetErrorMode, SEM_FAILCRITICALERRORS, SetErrorMode,
};
use windows_sys::Win32::System::LibraryLoader::{
    BeginUpdateResourceW, EndUpdateResourceW, GetModuleHandleW, GetProcAddress, UpdateResourceW,
};
use windows_sys::Win32::System::Power::SetThreadExecutionState;
use windows_sys::Win32::System::Registry::{
    HKEY, HKEY_CURRENT_USER, KEY_READ, RRF_RT_REG_DWORD, RRF_RT_REG_SZ, RegCloseKey, RegEnumKeyExW,
    RegGetValueW, RegOpenKeyExW,
};
use windows_sys::Win32::System::SystemInformation::{
    IMAGE_FILE_MACHINE, IMAGE_FILE_MACHINE_UNKNOWN, OSVERSIONINFOW,
};
use windows_sys::Win32::System::Threading::{
    GetCurrentProcess, GetProcessTimes, INFINITE, OpenProcess, PROCESS_ACCESS_RIGHTS,
    PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_SYNCHRONIZE, PROCESS_TERMINATE,
    QueryFullProcessImageNameW, TerminateProcess, WaitForSingleObject,
};
use windows_sys::Win32::UI::Shell::{
    FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Music, FOLDERID_Pictures, FOLDERID_Videos,
    KF_FLAG_DONT_VERIFY, SHCNE_UPDATEITEM, SHCNF_PATHW, SHChangeNotify, SHGetKnownFolderPath,
};
use windows_sys::core::{BOOL, GUID, PCWSTR, PWSTR, w};

use crate::destination::KnownFolder;
use crate::system_proxy::{ProxyEnable, RawInternetSettings};

#[cfg(test)]
use std::collections::BTreeMap;
#[cfg(test)]
use std::fmt::Display;
#[cfg(test)]
use std::sync::atomic::{AtomicU32, Ordering};
#[cfg(test)]
use windows_sys::Win32::Foundation::{
    ERROR_RESOURCE_LANG_NOT_FOUND, ERROR_RESOURCE_NAME_NOT_FOUND, ERROR_RESOURCE_TYPE_NOT_FOUND,
    FreeLibrary, HANDLE_FLAG_INHERIT, HMODULE, SetHandleInformation, WIN32_ERROR,
};
#[cfg(test)]
use windows_sys::Win32::Foundation::{GENERIC_ALL, GENERIC_WRITE};
#[cfg(test)]
use windows_sys::Win32::Security::{
    ACCESS_ALLOWED_ACE, ACE_HEADER, ACL, ACL_REVISION, AddAccessAllowedAce, CheckTokenMembership,
    CreateRestrictedToken, CreateWellKnownSid, DISABLE_MAX_PRIVILEGE, GetAce, GetLengthSid,
    GetSidSubAuthority, GetSidSubAuthorityCount, GetTokenInformation, InitializeAcl,
    InitializeSecurityDescriptor, PSID, SECURITY_ATTRIBUTES, SECURITY_DESCRIPTOR,
    SECURITY_MAX_SID_SIZE, SID_AND_ATTRIBUTES, SetSecurityDescriptorDacl, SetTokenInformation,
    TOKEN_ACCESS_MASK, TOKEN_ADJUST_DEFAULT, TOKEN_ASSIGN_PRIMARY, TOKEN_DEFAULT_DACL,
    TOKEN_DUPLICATE, TOKEN_INFORMATION_CLASS, TOKEN_MANDATORY_LABEL, TOKEN_QUERY, TOKEN_USER,
    TokenDefaultDacl, TokenElevationType, TokenElevationTypeDefault, TokenElevationTypeFull,
    TokenElevationTypeLimited, TokenIntegrityLevel, TokenSessionId, TokenUser, WELL_KNOWN_SID_TYPE,
    WinBuiltinAdministratorsSid, WinLocalSystemSid, WinMediumLabelSid,
};
#[cfg(test)]
use windows_sys::Win32::Storage::FileSystem::{DELETE, GetShortPathNameW, SetFileShortNameW};
#[cfg(test)]
use windows_sys::Win32::System::Diagnostics::Debug::SEM_NOGPFAULTERRORBOX;
#[cfg(test)]
use windows_sys::Win32::System::LibraryLoader::{
    FindResourceExW, LOAD_LIBRARY_AS_DATAFILE, LOAD_LIBRARY_AS_IMAGE_RESOURCE, LOAD_LIBRARY_FLAGS,
    LoadLibraryExW, LoadResource, LockResource, SizeofResource,
};
#[cfg(test)]
use windows_sys::Win32::System::Power::ES_CONTINUOUS;
#[cfg(test)]
use windows_sys::Win32::System::Registry::HKEY_LOCAL_MACHINE;
#[cfg(test)]
use windows_sys::Win32::System::StationsAndDesktops::{
    CloseDesktop, CloseWindowStation, CreateDesktopW, CreateWindowStationW,
    GetProcessWindowStation, GetThreadDesktop, GetUserObjectInformationW, HDESK, HWINSTA,
    SetProcessWindowStation, UOI_NAME,
};
#[cfg(test)]
use windows_sys::Win32::System::Threading::{
    CREATE_NO_WINDOW, CREATE_UNICODE_ENVIRONMENT, CreateProcessAsUserW,
    DeleteProcThreadAttributeList, EXTENDED_STARTUPINFO_PRESENT, GetCurrentThreadId,
    GetExitCodeProcess, InitializeProcThreadAttributeList, LPPROC_THREAD_ATTRIBUTE_LIST,
    OpenProcessToken, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, PROCESS_INFORMATION, STARTF_USESTDHANDLES,
    STARTUPINFOEXW, STARTUPINFOW, UpdateProcThreadAttribute,
};
#[cfg(test)]
use windows_sys::Win32::UI::WindowsAndMessaging::{CWF_CREATE_ONLY, WINSTA_ALL_ACCESS};

/// IsWow64Process2's signature. The call is looked up while running, since Windows 10 has it only
/// from version 1511 on, and a program that imported it would not start on a system without it.
type IsWow64Process2 =
    unsafe extern "system" fn(HANDLE, *mut IMAGE_FILE_MACHINE, *mut IMAGE_FILE_MACHINE) -> BOOL;

/// The registry key under the user's own that holds a key for each OneDrive account signed in.
const ONEDRIVE_ACCOUNTS_KEY: PCWSTR = w!(r"Software\Microsoft\OneDrive\Accounts");

/// The value of an account's key that names the folder OneDrive keeps in sync for it.
const USER_FOLDER_VALUE: PCWSTR = w!("UserFolder");

/// The most accounts' keys read, so that a key holding far more than any machine signs in to
/// cannot keep a judging reading.
const MAX_ONEDRIVE_ACCOUNTS: u32 = 64;

/// The UTF-16 units a registry key's name may hold, with room for the NUL after it.
const MAX_KEY_NAME_UNITS: usize = 256;

/// The most UTF-16 units a path may hold, the NUL after it included: a verbatim path of Windows'
/// greatest length.
const MAX_PATH_UNITS: usize = 32_768;

/// The most bytes an account's folder is read in: a verbatim path of Windows' greatest length.
const MAX_USER_FOLDER_BYTES: u32 = 2 * MAX_PATH_UNITS as u32;

/// The registry key under the user's own that holds the user's Internet Settings, the proxy among
/// them.
const INTERNET_SETTINGS_KEY: PCWSTR =
    w!(r"Software\Microsoft\Windows\CurrentVersion\Internet Settings");

/// The value of the Internet Settings that switches the proxy on.
const PROXY_ENABLE_VALUE: PCWSTR = w!("ProxyEnable");

/// The value of the Internet Settings that names the proxy.
const PROXY_SERVER_VALUE: PCWSTR = w!("ProxyServer");

/// The value of the Internet Settings that lists the hosts reached without the proxy.
const PROXY_OVERRIDE_VALUE: PCWSTR = w!("ProxyOverride");

/// The most bytes a proxy setting is read in: a value far longer than any proxy's address or
/// bypass list is not read at all.
const MAX_PROXY_VALUE_BYTES: u32 = 64 * 1024;

/// The rights a process is opened with to be waited for and asked when it started and which file
/// it runs.
const WAIT_RIGHTS: PROCESS_ACCESS_RIGHTS = PROCESS_SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION;

/// The longest wait WaitForSingleObject takes in milliseconds: one less than INFINITE, which
/// waits for ever.
const LONGEST_WAIT_MILLIS: u32 = INFINITE - 1;

/// The relative id that ends the integrity level's SID of a token at Medium integrity, a standard
/// user's: SECURITY_MANDATORY_MEDIUM_RID.
#[cfg(test)]
pub(crate) const MEDIUM_INTEGRITY: u32 = 0x2000;

/// SE_GROUP_INTEGRITY: the attribute a token's integrity level is set with.
#[cfg(test)]
const INTEGRITY_ATTRIBUTE: u32 = 0x20;

/// The 32-bit words of the largest SID, SECURITY_MAX_SID_SIZE bytes.
#[cfg(test)]
const SID_WORDS: usize = SECURITY_MAX_SID_SIZE as usize / size_of::<u32>();

/// The exit code a program started under a filtered token is given when it is ended.
#[cfg(test)]
const FILTERED_ENDED_EXIT_CODE: u32 = 1;

/// How long a program started under a filtered token is given to be gone once it is ended.
#[cfg(test)]
const FILTERED_ENDED_WAIT: Duration = Duration::from_secs(5);

/// What starts the name of a window station made for a program under a filtered token; this
/// process's id and a count follow, so that no two are named alike.
#[cfg(test)]
const FILTERED_STATION_PREFIX: &str = "GdlFilteredToken";

/// The name of the desktop made in a window station for a program under a filtered token.
#[cfg(test)]
const FILTERED_DESKTOP: &str = "Default";

/// How many window stations this process has made for programs under a filtered token: the count
/// in the next one's name.
#[cfg(test)]
static FILTERED_STATIONS: AtomicU32 = AtomicU32::new(0);

/// The most UTF-16 units a window station's or desktop's name is read in, the NUL after it
/// included: MAX_PATH, more than any such name holds.
#[cfg(test)]
const MAX_USER_OBJECT_NAME_UNITS: usize = 260;

/// SECURITY_DESCRIPTOR_REVISION: the revision a security descriptor is made in.
#[cfg(test)]
const SECURITY_DESCRIPTOR_REVISION: u32 = 1;

/// ACCESS_ALLOWED_ACE_TYPE: the type of an ACL's entry that grants rights.
#[cfg(test)]
const ACCESS_ALLOWED_ENTRY: u8 = 0;

/// The errors FindResourceExW gives for a resource that a program does not hold: none of its
/// type, none of its id among those of its type, or none in its language.
#[cfg(test)]
const ABSENT_RESOURCE: [WIN32_ERROR; 3] = [
    ERROR_RESOURCE_TYPE_NOT_FOUND,
    ERROR_RESOURCE_NAME_NOT_FOUND,
    ERROR_RESOURCE_LANG_NOT_FOUND,
];

/// A resource to set into a program: its type and its id, both numbers, the language it is kept
/// under, and what it holds.
pub(crate) struct Resource<'a> {
    /// The resource's type, such as an icon's image or an icon's group.
    pub(crate) kind: u16,
    /// The resource's id among those of its type.
    pub(crate) id: u16,
    /// The language the resource is kept under; 0 is the neutral one.
    pub(crate) language: u16,
    /// What the resource holds.
    pub(crate) bytes: &'a [u8],
}

/// Sets `resources` into the program at `executable`, beside every resource it holds already;
/// one of the same type, id and language is replaced. The program is rewritten only once all of
/// them are taken, and a refusal before that leaves it as it was. The program goes to the calls in
/// its verbatim form, so that it is not held to MAX_PATH.
pub(crate) fn update_resources(executable: &Path, resources: &[Resource<'_>]) -> io::Result<()> {
    let update = ResourceUpdate::begin(executable)?;
    for resource in resources {
        update.set(resource)?;
    }
    update.write()
}

/// A resource's type or id as the pointer-typed value that the resource calls take a number in,
/// as MAKEINTRESOURCE makes one: the number as an address, which the calls never read through.
fn numbered(number: u16) -> PCWSTR {
    ptr::without_provenance(usize::from(number))
}

/// An update of a program's resources that BeginUpdateResourceW began, whose resources borrow
/// bytes for `'a`. It is discarded when dropped, unless it was written.
struct ResourceUpdate<'a> {
    /// The update's handle.
    handle: HANDLE,
    /// The bytes of the resources set, which must outlast the update.
    resources: PhantomData<&'a [u8]>,
}

impl<'a> ResourceUpdate<'a> {
    /// Begins an update of the resources of the program at `executable`, which keeps those it
    /// holds.
    fn begin(executable: &Path) -> io::Result<ResourceUpdate<'a>> {
        let path = VerbatimPath::new(executable)?;
        // SAFETY: the path is a NUL-terminated UTF-16 string that `path` owns and keeps alive
        // until the call returns, which only reads it.
        let handle = unsafe { BeginUpdateResourceW(path.as_ptr(), FALSE) };
        if handle.is_null() {
            Err(io::Error::last_os_error())
        } else {
            Ok(ResourceUpdate {
                handle,
                resources: PhantomData,
            })
        }
    }

    /// Adds `resource` to the update.
    fn set(&self, resource: &Resource<'a>) -> io::Result<()> {
        let len = u32::try_from(resource.bytes.len()).map_err(|_| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                "a resource is longer than a program may hold one",
            )
        })?;
        // SAFETY: the handle came from a successful BeginUpdateResourceW and has not been ended;
        // the type and id are numbers in the pointer-typed form the call takes them in, which it
        // never reads through; and the bytes are `len` readable bytes, borrowed for `'a`, which
        // outlasts the update and so every use the system makes of them.
        let set = unsafe {
            UpdateResourceW(
                self.handle,
                numbered(resource.kind),
                numbered(resource.id),
                resource.language,
                resource.bytes.as_ptr().cast(),
                len,
            )
        };
        if set == 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }

    /// Rewrites the program with the update, and ends it.
    fn write(self) -> io::Result<()> {
        let update = ManuallyDrop::new(self);
        // SAFETY: the handle came from a successful BeginUpdateResourceW and is ended here alone,
        // since the update, kept from dropping, no longer discards it.
        let written = unsafe { EndUpdateResourceW(update.handle, FALSE) };
        if written == 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }
}

impl Drop for ResourceUpdate<'_> {
    fn drop(&mut self) {
        // SAFETY: the handle came from a successful BeginUpdateResourceW, belongs to this value
        // alone and is ended only here or in `write`, which keeps this from running.
        unsafe { EndUpdateResourceW(self.handle, TRUE) };
    }
}

/// The bytes of the resource of type `kind`, id `id` and language `language` that the program at
/// `executable` holds, or `None` when it holds none such. The program is loaded as data alone, so
/// none of its code runs and none of the libraries it names is loaded.
#[cfg(test)]
pub(crate) fn resource_bytes(
    executable: &Path,
    kind: u16,
    id: u16,
    language: u16,
) -> io::Result<Option<Vec<u8>>> {
    let module = DataModule::load(executable)?;
    // SAFETY: the module was loaded and stays loaded until `module` drops, after the last call
    // here; the type and id are numbers in the pointer-typed form the call takes them in, which
    // it never reads through.
    let found = unsafe { FindResourceExW(module.0, numbered(kind), numbered(id), language) };
    if found.is_null() {
        let error = io::Error::last_os_error();
        let absent = error
            .raw_os_error()
            .and_then(|code| u32::try_from(code).ok())
            .is_some_and(|code| ABSENT_RESOURCE.contains(&code));
        return if absent { Ok(None) } else { Err(error) };
    }
    // SAFETY: `found` names a resource of the loaded module.
    let len = unsafe { SizeofResource(module.0, found) };
    // SAFETY: the same.
    let data = unsafe { LoadResource(module.0, found) };
    if data.is_null() {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: `data` is the handle LoadResource gave for a resource of the loaded module.
    let at = unsafe { LockResource(data) };
    if at.is_null() {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: LockResource points at the resource's `len` bytes in the module's mapped image,
    // which stays mapped until `module` drops, after they are copied out here.
    let bytes = unsafe { std::slice::from_raw_parts(at.cast::<u8>(), len as usize) };
    Ok(Some(bytes.to_vec()))
}

/// A program loaded as data alone, for its resources, and freed when dropped.
#[cfg(test)]
struct DataModule(HMODULE);

#[cfg(test)]
impl DataModule {
    /// Loads the program at `executable` as an image of data, whose code never runs.
    fn load(executable: &Path) -> io::Result<DataModule> {
        load_unrun(
            executable,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE,
        )
        .map(DataModule)
    }
}

#[cfg(test)]
impl Drop for DataModule {
    fn drop(&mut self) {
        // SAFETY: the module came from a successful LoadLibraryExW, belongs to this value alone
        // and is freed only here.
        unsafe { FreeLibrary(self.0) };
    }
}

/// A program file mapped into this process as an image whose code never runs, as the system maps
/// the file of a program that runs: while it is mapped, the system renames the file but does not
/// delete it. Unmapped when dropped.
#[cfg(test)]
pub(crate) struct MappedImage(HMODULE);

/// Maps the program file at `path` as [`MappedImage`] says.
#[cfg(test)]
pub(crate) fn map_image(path: &Path) -> io::Result<MappedImage> {
    load_unrun(path, LOAD_LIBRARY_AS_IMAGE_RESOURCE).map(MappedImage)
}

#[cfg(test)]
impl Drop for MappedImage {
    fn drop(&mut self) {
        // SAFETY: the module came from a successful LoadLibraryExW, belongs to this value alone
        // and is freed only here.
        unsafe { FreeLibrary(self.0) };
    }
}

/// Loads the program at `path` with `flags`, which load it as data or as an image resource, so
/// that none of its code runs and nothing it imports is loaded; the caller frees the module. It
/// goes to the call in its verbatim form, so that it is not held to MAX_PATH.
#[cfg(test)]
fn load_unrun(path: &Path, flags: LOAD_LIBRARY_FLAGS) -> io::Result<HMODULE> {
    let verbatim = VerbatimPath::new(path)?;
    // SAFETY: the path is a NUL-terminated UTF-16 string that `verbatim` owns and keeps alive
    // until the call returns, which only reads it; the call takes no file handle, which is null
    // as it asks; and the flags, data or an image resource, run nothing of the file.
    let module = unsafe { LoadLibraryExW(verbatim.as_ptr(), ptr::null_mut(), flags) };
    if module.is_null() {
        Err(io::Error::last_os_error())
    } else {
        Ok(module)
    }
}

/// Tells the shell that the file at `path` has changed, so that Explorer draws it afresh rather
/// than from what it remembers, its icon above all. The shell names files by their plain paths, so
/// `path` goes to it made absolute and without a verbatim prefix. A path that cannot be made
/// absolute, or that holds a NUL, names no file the shell shows, and nothing is told.
pub(crate) fn notify_item_updated(path: &Path) {
    let Ok(absolute) = std::path::absolute(path) else {
        return;
    };
    let mut plain: Vec<u16> = strip_verbatim(&absolute)
        .as_os_str()
        .encode_wide()
        .collect();
    if plain.contains(&0) {
        return;
    }
    plain.push(0);
    // SAFETY: the path is a NUL-terminated UTF-16 string, as SHCNF_PATHW tells the call, that
    // `plain` owns and keeps alive until the call returns, which only reads it; this event names
    // no second item, which is then null.
    unsafe {
        SHChangeNotify(
            SHCNE_UPDATEITEM.cast_signed(),
            SHCNF_PATHW,
            plain.as_ptr().cast(),
            ptr::null(),
        );
    }
}

/// Asks the system, for the calling thread, what `flags` asks as SetThreadExecutionState takes
/// them, and gives back the thread's requests that were in force before; 0 when the system
/// refused. With ES_CONTINUOUS the flags replace the thread's lasting requests, which hold until
/// they are replaced again or the thread ends.
pub(crate) fn set_execution_state(flags: u32) -> u32 {
    // SAFETY: the call takes only flags, and changes nothing but the calling thread's requests.
    unsafe { SetThreadExecutionState(flags) }
}

/// The calling thread's lasting requests that the system stay awake, read by replacing them with
/// none and putting them straight back.
#[cfg(test)]
pub(crate) fn execution_state() -> u32 {
    let state = set_execution_state(ES_CONTINUOUS);
    set_execution_state(state | ES_CONTINUOUS);
    state
}

/// The system's own machine, as a program's header names one, which IsWow64Process2 gives
/// whatever machine this program was built for and the system runs it as: the processor's. `None`
/// when the system lacks the call, as Windows 10 before version 1511 does, or the call fails.
pub(crate) fn native_machine() -> Option<u16> {
    // SAFETY: the name is a NUL-terminated UTF-16 string that the macro keeps for the whole run,
    // and the call only reads it; kernel32 is loaded into every process, and the handle the call
    // gives for it takes no reference that would need freeing.
    let kernel32 = unsafe { GetModuleHandleW(windows_sys::core::w!("kernel32.dll")) };
    if kernel32.is_null() {
        return None;
    }
    // SAFETY: the handle names a module loaded for the whole run, and the name is a
    // NUL-terminated string that the macro keeps for the whole run, which the call only reads.
    let found = unsafe { GetProcAddress(kernel32, windows_sys::core::s!("IsWow64Process2")) }?;
    // SAFETY: what GetProcAddress found under that name is IsWow64Process2, whose signature is
    // the one it is given here; only the type of the pointer changes.
    let is_wow64_process2 = unsafe {
        std::mem::transmute::<unsafe extern "system" fn() -> isize, IsWow64Process2>(found)
    };
    let mut process = IMAGE_FILE_MACHINE_UNKNOWN;
    let mut native = IMAGE_FILE_MACHINE_UNKNOWN;
    // SAFETY: GetCurrentProcess gives a pseudo handle for this process, which needs no closing;
    // the two machines are written through references to locals that outlive the call.
    let answered = unsafe { is_wow64_process2(GetCurrentProcess(), &mut process, &mut native) };
    (answered != 0).then_some(native)
}

/// Adds SEM_FAILCRITICALERRORS to the process's error mode, keeping the rest of it, and leaves it
/// there: the system then fails a call that meets a critical error, such as a drive with no medium
/// in it, or a program that names a library it cannot find, rather than show a dialog and wait
/// for someone to close it. Every program the process starts inherits the mode, so a probed
/// program that cannot load ends at once with an error status. The mode is the whole process's,
/// so it is never put back, which would take it from another thread that relies on it.
pub(crate) fn fail_critical_errors() {
    // SAFETY: both calls take and give only flags, and change nothing but the process's error
    // mode.
    unsafe { SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS) };
}

/// The process's error mode.
#[cfg(test)]
pub(crate) fn error_mode() -> u32 {
    // SAFETY: the call takes nothing and only reads the process's error mode.
    unsafe { GetErrorMode() }
}

/// Adds SEM_NOGPFAULTERRORBOX to the process's error mode, keeping the rest of it, so that a test
/// program started in the system's default mode, where critical errors show a dialog, still never
/// shows one should it fault.
#[cfg(test)]
pub(crate) fn refuse_fault_dialogs() {
    // SAFETY: both calls take and give only flags, and change nothing but the process's error
    // mode.
    unsafe { SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX) };
}

/// The system's build number, such as 19045 for Windows 10 22H2, as RtlGetVersion gives it: no
/// manifest changes it, as a program's manifest, or its lack of one, changes what GetVersionExW
/// answers. 0 when the call fails.
pub(crate) fn windows_build() -> u32 {
    let mut info = OSVERSIONINFOW {
        dwOSVersionInfoSize: size_of::<OSVERSIONINFOW>() as u32,
        ..OSVERSIONINFOW::default()
    };
    // SAFETY: `info` is an OSVERSIONINFOW whose size field says so, which the call fills.
    let status = unsafe { RtlGetVersion(&mut info) };
    if status == STATUS_SUCCESS {
        info.dwBuildNumber
    } else {
        0
    }
}

/// Where the user's known folder `folder` is, as the shell names it, whether or not it is there:
/// `None` when the shell names none.
pub(crate) fn known_folder(folder: KnownFolder) -> Option<PathBuf> {
    let id: &GUID = match folder {
        KnownFolder::Documents => &FOLDERID_Documents,
        KnownFolder::Desktop => &FOLDERID_Desktop,
        KnownFolder::Pictures => &FOLDERID_Pictures,
        KnownFolder::Videos => &FOLDERID_Videos,
        KnownFolder::Music => &FOLDERID_Music,
    };
    let mut path: PWSTR = ptr::null_mut();
    // SAFETY: the id is a GUID that lives through the call, which only reads it; the token is
    // null, which names the calling user; and the call writes the address of a string it
    // allocates through the pointer it is given, to a local that outlives it.
    let answer = unsafe {
        SHGetKnownFolderPath(
            id,
            KF_FLAG_DONT_VERIFY.cast_unsigned(),
            ptr::null_mut(),
            &mut path,
        )
    };
    let found = (answer >= 0 && !path.is_null()).then(|| {
        // SAFETY: a successful call gives a NUL-terminated UTF-16 string, which stays allocated
        // until it is freed below, after it is copied out here.
        let units = unsafe { wide_string(path) };
        PathBuf::from(OsString::from_wide(units))
    });
    // SAFETY: the string came from SHGetKnownFolderPath, which leaves it to its caller to free
    // whether or not it succeeded, and is freed only here; freeing null does nothing.
    unsafe { CoTaskMemFree(path.cast_const().cast()) };
    found
}

/// The UTF-16 units of the NUL-terminated string at `text`, without the NUL.
///
/// # Safety
///
/// `text` must point at a NUL-terminated UTF-16 string that stays allocated, and unchanged, while
/// the units given back are used.
unsafe fn wide_string<'a>(text: *const u16) -> &'a [u16] {
    let mut len = 0;
    // SAFETY: every unit up to the NUL is part of the string, as the caller promises.
    while unsafe { *text.add(len) } != 0 {
        len += 1;
    }
    // SAFETY: the `len` units before the NUL are the string's, as the caller promises.
    unsafe { std::slice::from_raw_parts(text, len) }
}

/// The reparse tag of the entry at `path`, the entry itself and not what a link there leads to,
/// as GetFileInformationByHandleEx gives it: `None` when the entry is no reparse point. The entry
/// is opened to read nothing of what it holds, as std opens one to look at it.
pub(crate) fn reparse_tag(path: &Path) -> io::Result<Option<u32>> {
    let entry = OpenOptions::new()
        .access_mode(0)
        .custom_flags(FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS)
        .open(path)?;
    let mut info = FILE_ATTRIBUTE_TAG_INFO::default();
    // SAFETY: the handle is the open entry's, which lives through the call; the call writes a
    // FILE_ATTRIBUTE_TAG_INFO, of the size it is given, through a pointer to a local that
    // outlives it.
    let read = unsafe {
        GetFileInformationByHandleEx(
            entry.as_raw_handle(),
            FileAttributeTagInfo,
            (&raw mut info).cast(),
            size_of::<FILE_ATTRIBUTE_TAG_INFO>() as u32,
        )
    };
    if read == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT != 0).then_some(info.ReparseTag))
}

/// Whether the entry at `path`, the entry itself and not what a link there leads to, is the file
/// that `file` has open: the same file of the same volume, by the 128-bit ids that
/// GetFileInformationByHandleEx gives, or, where a file system gives none, by the volume serial
/// numbers and file indexes that GetFileInformationByHandle gives. Both are asked the same way, so
/// that ids of two kinds are never compared. The entry is opened to read nothing of what it holds,
/// as std opens one to look at it.
pub(crate) fn is_same_file(file: &File, path: &Path) -> io::Result<bool> {
    let entry = OpenOptions::new()
        .access_mode(0)
        .custom_flags(FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS)
        .open(path)?;
    if let (Ok(held), Ok(found)) = (file_id(file), file_id(&entry)) {
        return Ok(held == found);
    }
    Ok(file_index(file)? == file_index(&entry)?)
}

/// The volume serial number and the 128-bit file id of the file that `file` has open.
fn file_id(file: &File) -> io::Result<(u64, [u8; 16])> {
    let mut info = FILE_ID_INFO::default();
    // SAFETY: the handle is the open file's, which lives through the call; the call writes a
    // FILE_ID_INFO, of the size it is given, through a pointer to a local that outlives it.
    let read = unsafe {
        GetFileInformationByHandleEx(
            file.as_raw_handle(),
            FileIdInfo,
            (&raw mut info).cast(),
            size_of::<FILE_ID_INFO>() as u32,
        )
    };
    if read == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok((info.VolumeSerialNumber, info.FileId.Identifier))
}

/// The volume serial number and the file index, its high half first, of the file that `file` has
/// open.
fn file_index(file: &File) -> io::Result<(u32, u32, u32)> {
    let mut info = BY_HANDLE_FILE_INFORMATION::default();
    // SAFETY: the handle is the open file's, which lives through the call; the call fills the
    // BY_HANDLE_FILE_INFORMATION it is given, a local that outlives it.
    let read = unsafe { GetFileInformationByHandle(file.as_raw_handle(), &mut info) };
    if read == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok((
        info.dwVolumeSerialNumber,
        info.nFileIndexHigh,
        info.nFileIndexLow,
    ))
}

/// Another process, opened by its id to be waited for and asked about, and, when the system
/// granted the right, to be ended. The handle keeps the process's id from going to another process
/// while it is open, and is closed when dropped.
pub(crate) struct ProcessHandle {
    /// The process's handle.
    handle: OwnedHandle,
    /// Whether the handle carries the right to end the process.
    can_end: bool,
}

impl ProcessHandle {
    /// Whether the process may be ended through this handle.
    pub(crate) fn can_end(&self) -> bool {
        self.can_end
    }
}

/// What opening a process by its id found.
pub(crate) enum OpenedProcess {
    /// The process, open.
    Open(ProcessHandle),
    /// No process has the id: it ended, and nothing holds it any longer.
    Gone,
}

/// Opens the process whose id is `pid` to be waited for, asked when it started and which file it
/// runs, and ended, as OpenProcess opens one. A process the system will not let be ended, as it
/// refuses one of another account, is opened again without that right. The system refuses an id
/// no process has with ERROR_INVALID_PARAMETER, which is [`OpenedProcess::Gone`]; any other
/// refusal is its own error.
pub(crate) fn open_process(pid: u32) -> io::Result<OpenedProcess> {
    let refused = match open_with(pid, WAIT_RIGHTS | PROCESS_TERMINATE) {
        Ok(handle) => {
            return Ok(OpenedProcess::Open(ProcessHandle {
                handle,
                can_end: true,
            }));
        }
        Err(refused) => refused,
    };
    let refused = if os_error(&refused) == Some(ERROR_ACCESS_DENIED) {
        match open_with(pid, WAIT_RIGHTS) {
            Ok(handle) => {
                return Ok(OpenedProcess::Open(ProcessHandle {
                    handle,
                    can_end: false,
                }));
            }
            Err(refused) => refused,
        }
    } else {
        refused
    };
    if os_error(&refused) == Some(ERROR_INVALID_PARAMETER) {
        Ok(OpenedProcess::Gone)
    } else {
        Err(refused)
    }
}

/// The Windows error code `error` carries, when it carries one.
fn os_error(error: &io::Error) -> Option<u32> {
    error.raw_os_error().map(i32::cast_unsigned)
}

/// Opens the process whose id is `pid` with `rights`.
fn open_with(pid: u32, rights: PROCESS_ACCESS_RIGHTS) -> io::Result<OwnedHandle> {
    // SAFETY: the call takes only numbers, and a handle it gives is this process's own, which no
    // other value owns.
    let handle = unsafe { OpenProcess(rights, FALSE, pid) };
    if handle.is_null() {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: the handle was just opened, is valid, and is owned by nothing else, so the value made
    // from it alone closes it.
    Ok(unsafe { OwnedHandle::from_raw_handle(handle) })
}

/// When `process` was started, in hundreds of nanoseconds since 1601, as GetProcessTimes gives it:
/// as coarse as the system's clock tick.
pub(crate) fn process_created(process: &ProcessHandle) -> io::Result<u64> {
    created(process.handle.as_raw_handle())
}

/// When this process was started, as [`process_created`] gives it.
pub(crate) fn own_process_created() -> io::Result<u64> {
    // SAFETY: the call takes nothing and gives a pseudo handle for this process, which needs no
    // closing.
    created(unsafe { GetCurrentProcess() })
}

/// When the process `handle` names was started.
fn created(handle: HANDLE) -> io::Result<u64> {
    let mut creation = FILETIME::default();
    let mut exit = FILETIME::default();
    let mut kernel = FILETIME::default();
    let mut user = FILETIME::default();
    // SAFETY: the handle is open with a right to query the process, as GetProcessTimes requires,
    // and lives through the call; the call writes four FILETIMEs through references to locals that
    // outlive it.
    let read = unsafe { GetProcessTimes(handle, &mut creation, &mut exit, &mut kernel, &mut user) };
    if read == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok((u64::from(creation.dwHighDateTime) << 32) | u64::from(creation.dwLowDateTime))
}

/// Waits up to `limit` for `process` to end: `true` when it has ended, `false` when it still runs
/// at the limit. A limit longer than the call takes is cut to the longest it takes.
pub(crate) fn wait_process(process: &ProcessHandle, limit: Duration) -> io::Result<bool> {
    let millis = u32::try_from(limit.as_millis())
        .unwrap_or(LONGEST_WAIT_MILLIS)
        .min(LONGEST_WAIT_MILLIS);
    // SAFETY: the handle is open with the right to wait for the process, and lives through the
    // call, which takes it and a number.
    let waited = unsafe { WaitForSingleObject(process.handle.as_raw_handle(), millis) };
    if waited == WAIT_OBJECT_0 {
        Ok(true)
    } else if waited == WAIT_TIMEOUT {
        Ok(false)
    } else {
        Err(io::Error::last_os_error())
    }
}

/// The file `process` runs, by its path as the system names it, as QueryFullProcessImageNameW
/// gives it.
pub(crate) fn process_image(process: &ProcessHandle) -> io::Result<PathBuf> {
    let mut units = vec![0_u16; MAX_PATH_UNITS];
    let mut len = units.len() as u32;
    // SAFETY: the handle is open with the right to query the process, and lives through the call;
    // the buffer holds `len` units, which the call is told and writes no more than, and it writes
    // how many it wrote, without the NUL after them, through a reference to a local.
    let read = unsafe {
        QueryFullProcessImageNameW(
            process.handle.as_raw_handle(),
            PROCESS_NAME_WIN32,
            units.as_mut_ptr(),
            &mut len,
        )
    };
    if read == 0 {
        return Err(io::Error::last_os_error());
    }
    units.truncate(len as usize);
    Ok(PathBuf::from(OsString::from_wide(&units)))
}

/// Ends `process`, as TerminateProcess ends one, with `code` as its exit code. The system ends it
/// after the call returns: [`wait_process`] tells when it has.
pub(crate) fn end_process(process: &ProcessHandle, code: u32) -> io::Result<()> {
    // SAFETY: the handle is open, and lives through the call, which takes it and a number; a
    // handle without the right to end the process is refused, and nothing else is ended.
    let ended = unsafe { TerminateProcess(process.handle.as_raw_handle(), code) };
    if ended == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

/// Whether this process has a console, as GetConsoleWindow tells: a program it starts that is a
/// console program shares it when it has one, and is given one of its own otherwise unless it is
/// started without.
pub(crate) fn has_console() -> bool {
    // SAFETY: the call takes nothing and only reads which console, if any, this process is
    // attached to; the window it names is not used.
    !unsafe { GetConsoleWindow() }.is_null()
}

/// A program started under a filtered token, as [`spawn_filtered`] starts one; ended when dropped,
/// unless it has ended by itself, so that none outlives its test.
#[cfg(test)]
pub(crate) struct FilteredChild {
    /// The program's process.
    process: ProcessHandle,
    /// Where the program was started: see [`FilteredChild::place`].
    place: String,
    /// The window station and desktop made for the program, closed once it is ended.
    _desktop: Option<FilteredDesktop>,
}

#[cfg(test)]
impl FilteredChild {
    /// The session the program was started in, the elevation type of this process's token, and by
    /// name the window station and desktop this process runs on and those the program was given,
    /// for a test to tell when the program fails.
    pub(crate) fn place(&self) -> &str {
        &self.place
    }

    /// Waits up to `limit` for the program to end, and gives its exit code: an error of kind
    /// TimedOut when it still runs at the limit.
    pub(crate) fn wait(&self, limit: Duration) -> io::Result<u32> {
        if !wait_process(&self.process, limit)? {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "a program started under a filtered token still runs",
            ));
        }
        let mut code = 0;
        // SAFETY: the handle is open with the right to query the process, and lives through the
        // call, which writes the exit code through a reference to a local.
        let read = unsafe { GetExitCodeProcess(self.process.handle.as_raw_handle(), &mut code) };
        if read == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(code)
    }
}

#[cfg(test)]
impl Drop for FilteredChild {
    fn drop(&mut self) {
        if matches!(wait_process(&self.process, Duration::ZERO), Ok(false)) {
            let _ = end_process(&self.process, FILTERED_ENDED_EXIT_CODE);
            let _ = wait_process(&self.process, FILTERED_ENDED_WAIT);
        }
    }
}

/// Starts `program` with `args` under a filtered copy of this process's token, as a standard
/// user's token is filtered from an administrator's: every privilege but the one to pass through
/// folders taken away, the Administrators group kept only to deny, the integrity level lowered to
/// Medium, and the default DACL, which whatever the program makes without a descriptor of its own
/// takes, granting the token's user and SYSTEM, as a standard user's does. Kept from an
/// administrator's token, whose default DACL may grant Administrators and SYSTEM alone, it would
/// leave the program unable to open what it makes for itself as it starts: kernel32 fails to
/// initialise, and the program dies with STATUS_DLL_INIT_FAILED before its main. The programs it
/// starts inherit the token, and with it the DACL. The program runs with this process's
/// environment, `env` set over it, without a console window, and with both its output streams on
/// `output`, which is the one handle it inherits. The system refuses a caller that may not start a
/// program under another token with ERROR_PRIVILEGE_NOT_HELD. The arguments are taken only when
/// none is empty or holds a space, a tab or a quote, which need no quoting.
///
/// When this process is an administrator, the program starts on a window station and desktop
/// made for it, which grant the filtered token's user, and the programs it starts inherit them.
/// The station and desktop this process runs on may grant Administrators and not that user, or be
/// labelled above Medium, and refuse the filtered token: user32, which a program linking shell32
/// loads as it starts, then cannot connect to them, and the program dies with
/// STATUS_DLL_INIT_FAILED before its main. Only an administrator may name a window station, so a
/// program another caller starts runs on the caller's own.
#[cfg(test)]
pub(crate) fn spawn_filtered(
    program: &Path,
    args: &[OsString],
    env: &[(OsString, OsString)],
    output: &File,
) -> io::Result<FilteredChild> {
    let token = filtered_token()?;
    let desktop = if token_is_administrator()? {
        let user = token_information::<TOKEN_USER>(&token, TokenUser)?;
        // SAFETY: the call wrote a TOKEN_USER at the buffer's start, whose SID lies in the buffer,
        // which lives while the desktop is made.
        let sid = unsafe { user.as_ptr().cast::<TOKEN_USER>().read() }
            .User
            .Sid;
        // Said apart, so that a refusal here is never taken for the start's own.
        // SAFETY: the SID is a valid one, lying in the buffer, which lives through the call.
        let made = unsafe { FilteredDesktop::granting(sid) }.map_err(|error| {
            io::Error::new(
                error.kind(),
                format!("making the program's window station and desktop: {error}"),
            )
        })?;
        Some(made)
    } else {
        None
    };
    let own = own_desktop();
    let given = desktop
        .as_ref()
        .map_or_else(|| own.clone(), FilteredDesktop::name);
    let place = format!(
        "session {}, elevation {}, this process on {own}, the program on {given}",
        described(own_session()),
        described(own_elevation())
    );
    let desktop_name = desktop
        .as_ref()
        .map_or(ptr::null_mut(), |desktop| desktop.name.as_ptr().cast_mut());
    let application: Vec<u16> = program.as_os_str().encode_wide().chain([0]).collect();
    let mut line = command_line(program, args)?;
    let environment = environment_block(env);
    let output = output.try_clone()?;
    let inherited = output.as_raw_handle();
    // SAFETY: the handle is the clone's, which lives through the call; the call only marks it to
    // be inherited, and the clone, this process's own, is closed when dropped.
    let marked =
        unsafe { SetHandleInformation(inherited, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) };
    if marked == 0 {
        return Err(io::Error::last_os_error());
    }
    let mut list = InheritedHandles::naming(&inherited)?;
    let startup = STARTUPINFOEXW {
        StartupInfo: STARTUPINFOW {
            cb: size_of::<STARTUPINFOEXW>() as u32,
            lpDesktop: desktop_name,
            dwFlags: STARTF_USESTDHANDLES,
            hStdOutput: inherited,
            hStdError: inherited,
            ..STARTUPINFOW::default()
        },
        lpAttributeList: list.as_ptr(),
    };
    let mut started = PROCESS_INFORMATION::default();
    // SAFETY: the token is a primary token, open with the rights to be assigned to a process; the
    // program's path, the command line and the environment are NUL-terminated UTF-16 that their
    // vectors own and keep alive through the call, the command line writable as the call may
    // write it; the attribute list, initialized, names one inheritable handle that lives through
    // the call; the desktop's name, when one is given, is NUL-terminated UTF-16 that the desktop
    // owns and keeps alive through the call, which only reads it; and the call writes the
    // process's and its thread's handles and ids through a reference to a local.
    let created = unsafe {
        CreateProcessAsUserW(
            token.as_raw_handle(),
            application.as_ptr(),
            line.as_mut_ptr(),
            ptr::null(),
            ptr::null(),
            TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            environment.as_ptr().cast(),
            ptr::null(),
            &startup.StartupInfo,
            &mut started,
        )
    };
    if created == 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: the call gave this process both handles, open, and nothing else owns them, so the
    // values made from them alone close them.
    let (process, thread) = unsafe {
        (
            OwnedHandle::from_raw_handle(started.hProcess),
            OwnedHandle::from_raw_handle(started.hThread),
        )
    };
    drop(thread);
    Ok(FilteredChild {
        process: ProcessHandle {
            handle: process,
            can_end: true,
        },
        place,
        _desktop: desktop,
    })
}

/// A window station and a desktop in it, made by this process for a program it starts under a
/// filtered token, whose DACLs grant that token's user every right, as CreateProcessAsUserW wants
/// of the station and desktop a program starts on; closed when dropped.
#[cfg(test)]
struct FilteredDesktop {
    /// The window station.
    station: HWINSTA,
    /// The desktop in it.
    desktop: HDESK,
    /// Both by name, the station's, a backslash, then the desktop's, NUL-terminated, as a
    /// program's start names its desktop.
    name: Vec<u16>,
}

#[cfg(test)]
impl FilteredDesktop {
    /// A new window station and desktop whose DACLs grant `user` every right and nobody else any.
    ///
    /// # Safety
    ///
    /// `user` must point at a valid SID that stays allocated through the call.
    unsafe fn granting(user: PSID) -> io::Result<FilteredDesktop> {
        // SAFETY: the SID is a valid one that lives through the call, as the caller promises.
        let mut granted = unsafe { GrantedToOne::new(user) }?;
        let attributes = SECURITY_ATTRIBUTES {
            nLength: size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: granted.descriptor(),
            bInheritHandle: FALSE,
        };
        let count = FILTERED_STATIONS.fetch_add(1, Ordering::Relaxed);
        let station_name = format!("{FILTERED_STATION_PREFIX}-{}-{count}", std::process::id());
        let station_units: Vec<u16> = station_name.encode_utf16().chain([0]).collect();
        // SAFETY: the name is NUL-terminated UTF-16 that its vector owns and keeps alive through
        // the call; the attributes, and the descriptor and DACL they point at, live through the
        // call, which only reads them. A station of the name already there is refused, not shared.
        let station = unsafe {
            CreateWindowStationW(
                station_units.as_ptr(),
                CWF_CREATE_ONLY,
                WINSTA_ALL_ACCESS.cast_unsigned(),
                &attributes,
            )
        };
        if station.is_null() {
            return Err(io::Error::last_os_error());
        }
        let mut made = FilteredDesktop {
            station,
            desktop: ptr::null_mut(),
            name: format!("{station_name}\\{FILTERED_DESKTOP}")
                .encode_utf16()
                .chain([0])
                .collect(),
        };
        // A desktop is made in the window station of the process that makes it, so this process
        // stands on the new station while it makes the desktop, and goes back to its own at once.
        // SAFETY: the call takes nothing and gives this process's own window station, which the
        // system keeps open and nothing here closes.
        let own = unsafe { GetProcessWindowStation() };
        if own.is_null() {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: the station's handle is open, with every right a window station has.
        if unsafe { SetProcessWindowStation(station) } == 0 {
            return Err(io::Error::last_os_error());
        }
        let desktop_units: Vec<u16> = FILTERED_DESKTOP.encode_utf16().chain([0]).collect();
        // SAFETY: as for the station, the name and the attributes live through the call, which
        // only reads them; the device and its mode are null, as the call wants them.
        let desktop = unsafe {
            CreateDesktopW(
                desktop_units.as_ptr(),
                ptr::null(),
                ptr::null(),
                0,
                GENERIC_ALL,
                &attributes,
            )
        };
        let created = if desktop.is_null() {
            Err(io::Error::last_os_error())
        } else {
            made.desktop = desktop;
            Ok(())
        };
        // SAFETY: the handle is this process's own window station's, which is still open.
        if unsafe { SetProcessWindowStation(own) } == 0 {
            return Err(io::Error::last_os_error());
        }
        created.map(|()| made)
    }

    /// The station's name, a backslash, then the desktop's.
    fn name(&self) -> String {
        String::from_utf16_lossy(self.name.strip_suffix(&[0]).unwrap_or(&self.name))
    }
}

#[cfg(test)]
impl Drop for FilteredDesktop {
    fn drop(&mut self) {
        // SAFETY: each handle was made for this value alone and is closed only here, the desktop
        // only when it was made; a program still on them keeps them until it ends.
        unsafe {
            if !self.desktop.is_null() {
                CloseDesktop(self.desktop);
            }
            CloseWindowStation(self.station);
        }
    }
}

/// A DACL that grants each of a few SIDs GENERIC_ALL, which an object made with it maps to every
/// right it has, and nobody else anything; in words, so that it is aligned as an ACL wants.
#[cfg(test)]
struct FullAccessDacl(Vec<u32>);

#[cfg(test)]
impl FullAccessDacl {
    /// The DACL granting `sids`; it holds a copy of each.
    ///
    /// # Safety
    ///
    /// Each of `sids` must point at a valid SID that stays allocated through the call.
    unsafe fn granting(sids: &[PSID]) -> io::Result<FullAccessDacl> {
        // An access-allowed entry ends with its SID, whose first word is the entry's last field.
        let entries: usize = sids
            .iter()
            .map(|&sid| {
                // SAFETY: the SID is a valid one, as the caller promises, which the call only
                // reads.
                let sid_len = unsafe { GetLengthSid(sid) } as usize;
                size_of::<ACCESS_ALLOWED_ACE>() - size_of::<u32>() + sid_len
            })
            .sum();
        let len = size_of::<ACL>() + entries;
        let mut dacl = vec![0_u32; len.div_ceil(size_of::<u32>())];
        let dacl_len = (dacl.len() * size_of::<u32>()) as u32;
        // SAFETY: the storage, aligned for an ACL, holds `dacl_len` bytes, which the call is told
        // and writes no more than.
        if unsafe { InitializeAcl(dacl.as_mut_ptr().cast(), dacl_len, ACL_REVISION) } == 0 {
            return Err(io::Error::last_os_error());
        }
        for &sid in sids {
            // SAFETY: the ACL was initialized with room for an entry for each SID; the SID is
            // valid and lives through the call, as the caller promises, and the call copies it
            // into the entry.
            if unsafe {
                AddAccessAllowedAce(dacl.as_mut_ptr().cast(), ACL_REVISION, GENERIC_ALL, sid)
            } == 0
            {
                return Err(io::Error::last_os_error());
            }
        }
        Ok(FullAccessDacl(dacl))
    }

    /// The DACL, as the calls take one; it lies in the vector's heap storage, which does not move
    /// while the value is kept.
    fn as_ptr(&mut self) -> *mut ACL {
        self.0.as_mut_ptr().cast()
    }
}

/// A security descriptor whose DACL grants one SID GENERIC_ALL, which a window station or desktop
/// made with it maps to every right it has, and nobody else anything.
#[cfg(test)]
struct GrantedToOne {
    /// The DACL; the descriptor points at it.
    dacl: FullAccessDacl,
    /// The descriptor, in absolute form.
    descriptor: SECURITY_DESCRIPTOR,
}

#[cfg(test)]
impl GrantedToOne {
    /// The descriptor granting `sid`; the DACL holds a copy of it.
    ///
    /// # Safety
    ///
    /// `sid` must point at a valid SID that stays allocated through the call.
    unsafe fn new(sid: PSID) -> io::Result<GrantedToOne> {
        let mut granted = GrantedToOne {
            // SAFETY: the SID is a valid one that lives through the call, as the caller promises.
            dacl: unsafe { FullAccessDacl::granting(&[sid]) }?,
            descriptor: SECURITY_DESCRIPTOR::default(),
        };
        // SAFETY: the descriptor is storage of the structure's size, which the call initializes.
        if unsafe {
            InitializeSecurityDescriptor(granted.descriptor(), SECURITY_DESCRIPTOR_REVISION)
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        let dacl = granted.dacl.as_ptr();
        // SAFETY: the descriptor was initialized; the DACL it is given lies in the DACL's heap
        // storage, which does not move while the descriptor that points at it is kept beside it.
        if unsafe { SetSecurityDescriptorDacl(granted.descriptor(), TRUE, dacl, FALSE) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(granted)
    }

    /// The descriptor, as the calls take one.
    fn descriptor(&mut self) -> *mut core::ffi::c_void {
        ptr::from_mut(&mut self.descriptor).cast()
    }
}

/// By name, this process's window station, a backslash, then the calling thread's desktop, each
/// told as unknown with the reason when the system does not give it.
#[cfg(test)]
fn own_desktop() -> String {
    // SAFETY: neither call takes anything but the calling thread's id; each gives a handle the
    // system keeps open and nothing here closes, or null.
    let (station, desktop) = unsafe {
        (
            GetProcessWindowStation(),
            GetThreadDesktop(GetCurrentThreadId()),
        )
    };
    format!(
        "{}\\{}",
        described(user_object_name(station)),
        described(user_object_name(desktop))
    )
}

/// The name of the window station or desktop that `object` is a handle to, as
/// GetUserObjectInformationW gives it.
#[cfg(test)]
fn user_object_name(object: HANDLE) -> io::Result<String> {
    if object.is_null() {
        return Err(io::Error::last_os_error());
    }
    let mut units = vec![0_u16; MAX_USER_OBJECT_NAME_UNITS];
    let mut needed = 0;
    // SAFETY: the handle is open, and the call only reads it; the buffer holds `units.len()`
    // units, whose bytes the call is told and writes no more than, and it writes the bytes the
    // name needs through a reference to a local.
    let read = unsafe {
        GetUserObjectInformationW(
            object,
            UOI_NAME,
            units.as_mut_ptr().cast(),
            (units.len() * size_of::<u16>()) as u32,
            &mut needed,
        )
    };
    if read == 0 {
        return Err(io::Error::last_os_error());
    }
    let len = units
        .iter()
        .position(|&unit| unit == 0)
        .unwrap_or(units.len());
    Ok(String::from_utf16_lossy(&units[..len]))
}

/// The id of the session this process runs in, from its token.
#[cfg(test)]
fn own_session() -> io::Result<u32> {
    own_token_number(TokenSessionId)
}

/// The elevation type of this process's token: "full" for an elevated administrator's, "limited"
/// for the filtered half of an administrator's, "default" for a token that has no other half, as
/// with UAC off or for a standard user.
#[cfg(test)]
fn own_elevation() -> io::Result<&'static str> {
    let kind = own_token_number(TokenElevationType)?.cast_signed();
    let named = [
        (TokenElevationTypeFull, "full"),
        (TokenElevationTypeLimited, "limited"),
        (TokenElevationTypeDefault, "default"),
    ];
    Ok(named
        .iter()
        .find(|(known, _)| *known == kind)
        .map_or("unknown", |(_, name)| name))
}

/// What this process's token holds of `class`, which must be a 32-bit number, as the session's id
/// and the elevation type are.
#[cfg(test)]
fn own_token_number(class: TOKEN_INFORMATION_CLASS) -> io::Result<u32> {
    let token = own_token(TOKEN_QUERY)?;
    let mut number = 0_u32;
    let mut len = 0;
    // SAFETY: the token is open to be queried; the call writes a 32-bit number into a local of
    // that size, which it is told, and the bytes it wrote through a reference to a local.
    let read = unsafe {
        GetTokenInformation(
            token.as_raw_handle(),
            class,
            ptr::from_mut(&mut number).cast(),
            size_of::<u32>() as u32,
            &mut len,
        )
    };
    if read == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(number)
}

/// `answer` as a test reports it: the value, or "unknown" and the reason.
#[cfg(test)]
fn described<T: Display>(answer: io::Result<T>) -> String {
    match answer {
        Ok(value) => value.to_string(),
        Err(error) => format!("unknown ({error})"),
    }
}

/// What `token` holds of `class`, as GetTokenInformation gives it: a `T` at the start of storage
/// aligned for any of the structures the call writes, followed by what the `T` points at.
#[cfg(test)]
fn token_information<T>(
    token: &OwnedHandle,
    class: TOKEN_INFORMATION_CLASS,
) -> io::Result<Vec<usize>> {
    let mut len = 0;
    // SAFETY: the token is open to be queried; no buffer is given, which the call allows, and it
    // writes the bytes it needs through a reference to a local.
    unsafe { GetTokenInformation(token.as_raw_handle(), class, ptr::null_mut(), 0, &mut len) };
    if (len as usize) < size_of::<T>() {
        return Err(io::Error::last_os_error());
    }
    let mut buffer = vec![0_usize; (len as usize).div_ceil(size_of::<usize>())];
    // SAFETY: as above, and the buffer holds at least `len` bytes, which the call is told and
    // writes no more than.
    let read = unsafe {
        GetTokenInformation(
            token.as_raw_handle(),
            class,
            buffer.as_mut_ptr().cast(),
            len,
            &mut len,
        )
    };
    if read == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(buffer)
}

/// The bytes of the SID at `sid`.
///
/// # Safety
///
/// `sid` must point at a valid SID that stays allocated through the call.
#[cfg(test)]
unsafe fn sid_bytes(sid: PSID) -> Vec<u8> {
    // SAFETY: the SID is a valid one that lives through the call, as the caller promises; the
    // call only reads it, and it holds as many bytes as the call gives, which are copied here.
    unsafe { std::slice::from_raw_parts(sid.cast::<u8>(), GetLengthSid(sid) as usize) }.to_vec()
}

/// The SID of the user `token` names, by its bytes.
#[cfg(test)]
fn token_user_bytes(token: &OwnedHandle) -> io::Result<Vec<u8>> {
    let user_info = token_information::<TOKEN_USER>(token, TokenUser)?;
    // SAFETY: the call wrote a TOKEN_USER at the buffer's start, whose SID lies in the buffer,
    // which lives while the SID is copied.
    let user = unsafe { user_info.as_ptr().cast::<TOKEN_USER>().read() }
        .User
        .Sid;
    // SAFETY: the SID is a valid one, lying in the buffer, which lives through the call.
    Ok(unsafe { sid_bytes(user) })
}

/// Each entry of `token`'s default DACL, by the bytes of the SID it names and the rights it
/// grants: none when the token has no default DACL, and an error for an entry that does not
/// grant, such as one that denies.
#[cfg(test)]
fn default_dacl_grants(token: &OwnedHandle) -> io::Result<Vec<(Vec<u8>, u32)>> {
    let buffer = token_information::<TOKEN_DEFAULT_DACL>(token, TokenDefaultDacl)?;
    // SAFETY: the call wrote a TOKEN_DEFAULT_DACL at the buffer's start, whose DACL, when the
    // token has one, lies in the buffer, which lives while the DACL is read.
    let dacl = unsafe { buffer.as_ptr().cast::<TOKEN_DEFAULT_DACL>().read() }.DefaultDacl;
    if dacl.is_null() {
        return Ok(Vec::new());
    }
    // SAFETY: the DACL is a valid one, whose header is read.
    let count = unsafe { dacl.read() }.AceCount;
    let mut grants = Vec::new();
    for index in 0..u32::from(count) {
        let mut entry: *mut core::ffi::c_void = ptr::null_mut();
        // SAFETY: the DACL is a valid one holding `count` entries; the call writes a pointer to
        // the entry, which lies in the DACL, through a reference to a local.
        if unsafe { GetAce(dacl, index, &mut entry) } == 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: every entry begins with its header, which lies in the DACL.
        let kind = unsafe { entry.cast::<ACE_HEADER>().read() }.AceType;
        if kind != ACCESS_ALLOWED_ENTRY {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("the default DACL's entry {index} is of type {kind}, which grants nothing"),
            ));
        }
        let allowed = entry.cast::<ACCESS_ALLOWED_ACE>();
        // SAFETY: an access-allowed entry is laid out as ACCESS_ALLOWED_ACE, its SID beginning at
        // the field that ends it and lying in the entry, which lies in the DACL.
        let (mask, sid) = unsafe { ((*allowed).Mask, (&raw mut (*allowed).SidStart).cast()) };
        // SAFETY: the entry's SID is a valid one, lying in the DACL, which lies in the buffer,
        // which lives through the call.
        grants.push((unsafe { sid_bytes(sid) }, mask));
    }
    Ok(grants)
}

/// A filtered copy of this process's token, as [`spawn_filtered`] describes it: a primary token
/// that may be assigned to a program.
#[cfg(test)]
fn filtered_token() -> io::Result<OwnedHandle> {
    let own =
        own_token(TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT)?;
    let administrators = WellKnownSid::of(WinBuiltinAdministratorsSid)?;
    let deny_only = SID_AND_ATTRIBUTES {
        Sid: administrators.sid(),
        Attributes: 0,
    };
    let mut restricted: HANDLE = ptr::null_mut();
    // SAFETY: the token is open with the right to be duplicated; the one SID to keep only to deny
    // lives through the call, which only reads it; no privilege or restricting SID is listed; and
    // the call writes the new token's handle through a reference to a local.
    let made = unsafe {
        CreateRestrictedToken(
            own.as_raw_handle(),
            DISABLE_MAX_PRIVILEGE,
            1,
            &deny_only,
            0,
            ptr::null(),
            0,
            ptr::null(),
            &mut restricted,
        )
    };
    if made == 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: the handle was just made, is valid, and is owned by nothing else.
    let restricted = unsafe { OwnedHandle::from_raw_handle(restricted) };
    let medium = WellKnownSid::of(WinMediumLabelSid)?;
    let label = TOKEN_MANDATORY_LABEL {
        Label: SID_AND_ATTRIBUTES {
            Sid: medium.sid(),
            Attributes: INTEGRITY_ATTRIBUTE,
        },
    };
    // SAFETY: the SID is a valid one, which the call only reads.
    let len = size_of::<TOKEN_MANDATORY_LABEL>() as u32 + unsafe { GetLengthSid(medium.sid()) };
    // SAFETY: the new token's handle carries the rights of the one it was made from, the right to
    // change its defaults among them; the label and the SID it points at live through the call,
    // which only reads them.
    let set = unsafe {
        SetTokenInformation(
            restricted.as_raw_handle(),
            TokenIntegrityLevel,
            ptr::from_ref(&label).cast(),
            len,
        )
    };
    if set == 0 {
        return Err(io::Error::last_os_error());
    }
    // The new token keeps the default DACL of the one it was made from, which an administrator's
    // may grant to Administrators and SYSTEM alone: this token holds Administrators only to deny,
    // so the DACL is made to grant the token's own user, and SYSTEM, instead.
    let user_info = token_information::<TOKEN_USER>(&restricted, TokenUser)?;
    // SAFETY: the call wrote a TOKEN_USER at the buffer's start, whose SID lies in the buffer,
    // which lives while the DACL is made.
    let user = unsafe { user_info.as_ptr().cast::<TOKEN_USER>().read() }
        .User
        .Sid;
    let system = WellKnownSid::of(WinLocalSystemSid)?;
    // SAFETY: both SIDs are valid ones, the user's lying in its buffer and SYSTEM's in its value,
    // which both live through the call.
    let mut dacl = unsafe { FullAccessDacl::granting(&[user, system.sid()]) }?;
    let default = TOKEN_DEFAULT_DACL {
        DefaultDacl: dacl.as_ptr(),
    };
    // SAFETY: the new token's handle carries the right to change its defaults; the structure and
    // the DACL it points at live through the call, which copies the DACL into the token.
    let set = unsafe {
        SetTokenInformation(
            restricted.as_raw_handle(),
            TokenDefaultDacl,
            ptr::from_ref(&default).cast(),
            size_of::<TOKEN_DEFAULT_DACL>() as u32,
        )
    };
    if set == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(restricted)
}

/// This process's own token, opened with `rights`.
#[cfg(test)]
fn own_token(rights: TOKEN_ACCESS_MASK) -> io::Result<OwnedHandle> {
    let mut token: HANDLE = ptr::null_mut();
    // SAFETY: the pseudo handle of this process needs no closing; the call writes the token's
    // handle through a reference to a local.
    let opened = unsafe { OpenProcessToken(GetCurrentProcess(), rights, &mut token) };
    if opened == 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: the handle was just opened, is valid, and is owned by nothing else.
    Ok(unsafe { OwnedHandle::from_raw_handle(token) })
}

/// Whether this process's token holds the Administrators group to grant, as CheckTokenMembership
/// tells: a group kept only to deny is not held so.
#[cfg(test)]
pub(crate) fn token_is_administrator() -> io::Result<bool> {
    let administrators = WellKnownSid::of(WinBuiltinAdministratorsSid)?;
    let mut member: BOOL = FALSE;
    // SAFETY: no token names the calling thread's own; the SID lives through the call, which only
    // reads it; the answer is written through a reference to a local.
    let checked =
        unsafe { CheckTokenMembership(ptr::null_mut(), administrators.sid(), &mut member) };
    if checked == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(member != FALSE)
}

/// The integrity level of this process's token: the relative id that ends its integrity SID, such
/// as [`MEDIUM_INTEGRITY`].
#[cfg(test)]
pub(crate) fn token_integrity() -> io::Result<u32> {
    let token = own_token(TOKEN_QUERY)?;
    let buffer = token_information::<TOKEN_MANDATORY_LABEL>(&token, TokenIntegrityLevel)?;
    // SAFETY: the call wrote a TOKEN_MANDATORY_LABEL at the buffer's start, whose SID lies in the
    // buffer, which lives while it is read.
    let sid = unsafe { buffer.as_ptr().cast::<TOKEN_MANDATORY_LABEL>().read() }
        .Label
        .Sid;
    // SAFETY: the SID is a valid one, whose count of relative ids the call points at.
    let count = unsafe { *GetSidSubAuthorityCount(sid) };
    let last = u32::from(count).checked_sub(1).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            "an integrity level's SID holds no relative id",
        )
    })?;
    // SAFETY: the SID holds `count` relative ids, of which the call points at the last.
    Ok(unsafe { *GetSidSubAuthority(sid, last) })
}

/// A well-known SID, in storage of the largest SID's size aligned as a SID is.
#[cfg(test)]
struct WellKnownSid([u32; SID_WORDS]);

#[cfg(test)]
impl WellKnownSid {
    /// The well-known SID `kind`, of no domain.
    fn of(kind: WELL_KNOWN_SID_TYPE) -> io::Result<WellKnownSid> {
        let mut sid = WellKnownSid([0; SID_WORDS]);
        let mut len = SECURITY_MAX_SID_SIZE;
        // SAFETY: the storage holds `len` bytes, which the call is told and writes no more than;
        // no domain is given, which the well-known SIDs used here need none of.
        let made = unsafe {
            CreateWellKnownSid(kind, ptr::null_mut(), sid.0.as_mut_ptr().cast(), &mut len)
        };
        if made == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(sid)
    }

    /// The SID, as the calls that only read one take it.
    fn sid(&self) -> PSID {
        self.0.as_ptr().cast_mut().cast()
    }

    /// The SID's bytes.
    fn bytes(&self) -> Vec<u8> {
        // SAFETY: the value holds a valid SID, which lives while it is copied.
        unsafe { sid_bytes(self.sid()) }
    }
}

/// The attribute list of a program to start that names the one handle it inherits, which lives at
/// least as long; deleted when dropped.
#[cfg(test)]
struct InheritedHandles<'a> {
    /// The list's storage, in words so that it is aligned as the list wants.
    storage: Vec<usize>,
    /// The handle the list names.
    named: PhantomData<&'a HANDLE>,
}

#[cfg(test)]
impl<'a> InheritedHandles<'a> {
    /// A list naming `handle`, which must be inheritable.
    fn naming(handle: &'a HANDLE) -> io::Result<InheritedHandles<'a>> {
        let mut size = 0;
        // SAFETY: no list is given, which asks only the size a list of one attribute takes; the
        // call writes it through a reference to a local.
        unsafe { InitializeProcThreadAttributeList(ptr::null_mut(), 1, 0, &mut size) };
        if size == 0 {
            return Err(io::Error::last_os_error());
        }
        let mut storage = vec![0_usize; size.div_ceil(size_of::<usize>())];
        // SAFETY: the storage holds at least `size` bytes, which the call is told and writes no
        // more than.
        let initialized = unsafe {
            InitializeProcThreadAttributeList(storage.as_mut_ptr().cast(), 1, 0, &mut size)
        };
        if initialized == 0 {
            return Err(io::Error::last_os_error());
        }
        let mut list = InheritedHandles {
            storage,
            named: PhantomData,
        };
        // SAFETY: the list was initialized for one attribute; its value is the one handle, which
        // the list reads from where it lies, and which outlives the list.
        let updated = unsafe {
            UpdateProcThreadAttribute(
                list.as_ptr(),
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST as usize,
                ptr::from_ref(handle).cast(),
                size_of::<HANDLE>(),
                ptr::null_mut(),
                ptr::null(),
            )
        };
        if updated == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(list)
    }

    /// The list, as the calls take it.
    fn as_ptr(&mut self) -> LPPROC_THREAD_ATTRIBUTE_LIST {
        self.storage.as_mut_ptr().cast()
    }
}

#[cfg(test)]
impl Drop for InheritedHandles<'_> {
    fn drop(&mut self) {
        // SAFETY: the list was initialized, and is deleted only here.
        unsafe { DeleteProcThreadAttributeList(self.as_ptr()) };
    }
}

/// The command line that starts `program` with `args`, NUL-terminated: the program in quotes,
/// then each argument after a space. An argument that would need quoting is refused.
#[cfg(test)]
fn command_line(program: &Path, args: &[OsString]) -> io::Result<Vec<u16>> {
    const QUOTE: u16 = b'"' as u16;
    const SPACE: u16 = b' ' as u16;
    let needs_quoting =
        |unit: &u16| char::from_u32(u32::from(*unit)).is_none_or(|c| c.is_whitespace() || c == '"');
    let mut line = vec![QUOTE];
    line.extend(program.as_os_str().encode_wide());
    line.push(QUOTE);
    for arg in args {
        let units: Vec<u16> = arg.encode_wide().collect();
        if units.is_empty() || units.iter().any(needs_quoting) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("the argument {arg:?} would need quoting"),
            ));
        }
        line.push(SPACE);
        line.extend(units);
    }
    line.push(0);
    Ok(line)
}

/// The environment block of this process's environment with `env` set over it, as
/// CREATE_UNICODE_ENVIRONMENT reads one: each `name=value` NUL-terminated, sorted by name without
/// regard to ASCII letter case, which also tells two spellings of one name for the same, and a NUL
/// after the last.
#[cfg(test)]
fn environment_block(env: &[(OsString, OsString)]) -> Vec<u16> {
    let mut variables = BTreeMap::new();
    for (name, value) in std::env::vars_os().chain(env.iter().cloned()) {
        let key: Vec<u16> = name.to_ascii_uppercase().encode_wide().collect();
        variables.insert(key, (name, value));
    }
    let mut block = Vec::new();
    for (name, value) in variables.values() {
        block.extend(name.encode_wide());
        block.push(u16::from(b'='));
        block.extend(value.encode_wide());
        block.push(0);
    }
    if block.is_empty() {
        block.push(0);
    }
    block.push(0);
    block
}

/// The path of the entry at `path`, which must be there, each name on it as the folder above it
/// holds it in full, as GetLongPathNameW gives it: a short alias, such as PROGRA~1, gives way to
/// the name it stands for. The path goes to the call in its verbatim form, so that it is not held
/// to MAX_PATH, and comes back in it.
pub(crate) fn long_path(path: &Path) -> io::Result<PathBuf> {
    let path = VerbatimPath::new(path)?;
    let mut units = vec![0_u16; MAX_PATH_UNITS];
    // SAFETY: the path is a NUL-terminated UTF-16 string that `path` owns and keeps alive until
    // the call returns, which only reads it; the buffer holds `units.len()` units, which the call
    // is told and writes no more than.
    let len = unsafe { GetLongPathNameW(path.as_ptr(), units.as_mut_ptr(), units.len() as u32) };
    let len = len as usize;
    if len == 0 {
        return Err(io::Error::last_os_error());
    }
    if len >= units.len() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "a path's full form is longer than any path",
        ));
    }
    units.truncate(len);
    Ok(PathBuf::from(OsString::from_wide(&units)))
}

/// The path of the entry at `path`, which must be there, each name on it as its short alias where
/// it has one, as GetShortPathNameW gives it.
#[cfg(test)]
pub(crate) fn short_path(path: &Path) -> io::Result<PathBuf> {
    let path = VerbatimPath::new(path)?;
    let mut units = vec![0_u16; MAX_PATH_UNITS];
    // SAFETY: as in `long_path`.
    let len = unsafe { GetShortPathNameW(path.as_ptr(), units.as_mut_ptr(), units.len() as u32) };
    let len = len as usize;
    if len == 0 || len >= units.len() {
        return Err(io::Error::last_os_error());
    }
    units.truncate(len);
    Ok(PathBuf::from(OsString::from_wide(&units)))
}

/// Gives the folder at `path` the short alias `alias`, as SetFileShortNameW does, which a process
/// may ask only with the restore privilege enabled, and only of a file system that keeps such
/// aliases.
#[cfg(test)]
pub(crate) fn set_short_name(path: &Path, alias: &str) -> io::Result<()> {
    let folder = OpenOptions::new()
        .access_mode(GENERIC_WRITE | DELETE)
        .custom_flags(FILE_FLAG_BACKUP_SEMANTICS)
        .open(path)?;
    let mut name: Vec<u16> = alias.encode_utf16().collect();
    name.push(0);
    // SAFETY: the handle is the open folder's, which lives through the call; the alias is a
    // NUL-terminated UTF-16 string that `name` owns and keeps alive until the call returns, which
    // only reads it.
    let set = unsafe { SetFileShortNameW(folder.as_raw_handle(), name.as_ptr()) };
    if set == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

/// The folders that the OneDrive accounts signed in keep in sync, as the registry names them under
/// the user's own key: none when OneDrive has no account there. An account whose folder cannot be
/// read, or names none, gives none.
pub(crate) fn onedrive_account_folders() -> Vec<PathBuf> {
    let Ok(accounts) = RegistryKey::open_for_user(ONEDRIVE_ACCOUNTS_KEY) else {
        return Vec::new();
    };
    let mut folders = Vec::new();
    for index in 0..MAX_ONEDRIVE_ACCOUNTS {
        match accounts.subkey(index) {
            Subkey::Named(name) => folders.extend(accounts.user_folder(&name)),
            Subkey::Unread => {}
            Subkey::End => break,
        }
    }
    folders
}

/// The proxy values of the user's Internet Settings, as the registry holds them: the switch read
/// as a number, or as text where it is a string, and the server and bypass list as text. A value
/// the key lacks, or holds as another type or longer than [`MAX_PROXY_VALUE_BYTES`], is `None`;
/// text that is not Unicode is kept with its unpaired surrogates replaced, so that a proxy named
/// so is still gone through, and fails, rather than passed over for a direct request. A key that
/// cannot be opened is the system's own error.
pub(crate) fn internet_settings_values() -> io::Result<RawInternetSettings> {
    let settings = RegistryKey::open_for_user(INTERNET_SETTINGS_KEY)?;
    let text = |value| {
        settings
            .text(ptr::null(), value, MAX_PROXY_VALUE_BYTES)
            .map(|units| String::from_utf16_lossy(&units))
    };
    Ok(RawInternetSettings {
        enable: settings.switch(PROXY_ENABLE_VALUE),
        server: text(PROXY_SERVER_VALUE),
        bypass: text(PROXY_OVERRIDE_VALUE),
    })
}

/// A registry key opened to read, and closed when dropped.
struct RegistryKey(HKEY);

/// What a registry key holds at an index of its subkeys.
enum Subkey {
    /// The subkey's name, NUL-terminated.
    Named(Vec<u16>),
    /// A subkey whose name could not be read.
    Unread,
    /// No subkey: the index is past the last.
    End,
}

impl RegistryKey {
    /// Opens the key at `path` under the user's own key, to read.
    fn open_for_user(path: PCWSTR) -> io::Result<RegistryKey> {
        RegistryKey::open(HKEY_CURRENT_USER, path)
    }

    /// Opens the key at `path` under `root`, one of the predefined keys, to read.
    fn open(root: HKEY, path: PCWSTR) -> io::Result<RegistryKey> {
        let mut key: HKEY = ptr::null_mut();
        // SAFETY: the root is a predefined handle, always open; the path is a NUL-terminated
        // UTF-16 string that lives for the whole run, which the call only reads; and the call
        // writes the key it opens through a pointer to a local that outlives it.
        let status = unsafe { RegOpenKeyExW(root, path, 0, KEY_READ, &mut key) };
        if status == ERROR_SUCCESS {
            Ok(RegistryKey(key))
        } else {
            Err(io::Error::from_raw_os_error(status.cast_signed()))
        }
    }

    /// The subkey at `index`, as RegEnumKeyExW lists them.
    fn subkey(&self, index: u32) -> Subkey {
        let mut name = vec![0_u16; MAX_KEY_NAME_UNITS];
        let mut len = MAX_KEY_NAME_UNITS as u32;
        // SAFETY: the key is open; the name's buffer holds `len` units, which the call is told
        // and writes no more than; and the class, its length and the time, not asked for, are
        // null, which the call allows.
        let status = unsafe {
            RegEnumKeyExW(
                self.0,
                index,
                name.as_mut_ptr(),
                &mut len,
                ptr::null(),
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
            )
        };
        if status == ERROR_NO_MORE_ITEMS {
            return Subkey::End;
        }
        let len = len as usize;
        if status != ERROR_SUCCESS || len >= MAX_KEY_NAME_UNITS {
            return Subkey::Unread;
        }
        name.truncate(len);
        name.push(0);
        Subkey::Named(name)
    }

    /// The folder that the account whose subkey is named `account`, NUL-terminated, keeps in
    /// sync: its string value [`USER_FOLDER_VALUE`]. `None` when it cannot be read, is empty, or
    /// is longer than any path.
    fn user_folder(&self, account: &[u16]) -> Option<PathBuf> {
        let folder = self.text(account.as_ptr(), USER_FOLDER_VALUE, MAX_USER_FOLDER_BYTES)?;
        (!folder.is_empty()).then(|| PathBuf::from(OsString::from_wide(&folder)))
    }

    /// The units of the string value `value` of the subkey named `subkey`, or of this key itself
    /// when `subkey` is null, up to its first NUL: sized by a first read, and `None` when the
    /// value is missing, is no string, needs more than `max_bytes` or cannot be read.
    fn text(&self, subkey: PCWSTR, value: PCWSTR, max_bytes: u32) -> Option<Vec<u16>> {
        let mut bytes = 0_u32;
        // SAFETY: the key is open; the subkey's name, when there is one, and the value's are
        // NUL-terminated UTF-16 strings that outlive the call, which only reads them, and a null
        // subkey names the key itself; no data is asked for, which the call allows, and it writes
        // the bytes the value needs through a pointer to a local.
        let status = unsafe {
            RegGetValueW(
                self.0,
                subkey,
                value,
                RRF_RT_REG_SZ,
                ptr::null_mut(),
                ptr::null_mut(),
                &mut bytes,
            )
        };
        if status != ERROR_SUCCESS || bytes > max_bytes {
            return None;
        }
        if bytes == 0 {
            return Some(Vec::new());
        }
        let mut units = vec![0_u16; (bytes as usize).div_ceil(2)];
        let mut read = (units.len() * 2) as u32;
        // SAFETY: as above, and the buffer holds `read` bytes, which the call is told and writes
        // no more than.
        let status = unsafe {
            RegGetValueW(
                self.0,
                subkey,
                value,
                RRF_RT_REG_SZ,
                ptr::null_mut(),
                units.as_mut_ptr().cast(),
                &mut read,
            )
        };
        if status != ERROR_SUCCESS {
            return None;
        }
        units.truncate(read as usize / 2);
        let end = units
            .iter()
            .position(|&unit| unit == 0)
            .unwrap_or(units.len());
        units.truncate(end);
        Some(units)
    }

    /// The number value `value` of this key itself: `None` when it is missing, is no number or
    /// cannot be read.
    fn number(&self, value: PCWSTR) -> Option<u32> {
        let mut number = 0_u32;
        let mut bytes = size_of::<u32>() as u32;
        // SAFETY: the key is open; the value's name is a NUL-terminated UTF-16 string that
        // outlives the call, which only reads it, and a null subkey names the key itself; and the
        // call writes a number of the `bytes` it is told, and their count, through pointers to
        // locals that outlive it.
        let status = unsafe {
            RegGetValueW(
                self.0,
                ptr::null(),
                value,
                RRF_RT_REG_DWORD,
                ptr::null_mut(),
                (&raw mut number).cast(),
                &mut bytes,
            )
        };
        (status == ERROR_SUCCESS && bytes as usize == size_of::<u32>()).then_some(number)
    }

    /// The switch value `value` of this key itself: the number Windows writes, or text where it is
    /// a string. `None` when it is missing, is neither, or cannot be read.
    fn switch(&self, value: PCWSTR) -> Option<ProxyEnable> {
        self.number(value).map(ProxyEnable::Number).or_else(|| {
            self.text(ptr::null(), value, MAX_PROXY_VALUE_BYTES)
                .map(|units| ProxyEnable::Text(String::from_utf16_lossy(&units)))
        })
    }
}

impl Drop for RegistryKey {
    fn drop(&mut self) {
        // SAFETY: the key came from a successful RegOpenKeyExW, belongs to this value alone and
        // is closed only here.
        unsafe { RegCloseKey(self.0) };
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io;
    use std::os::windows::fs::OpenOptionsExt;
    use std::ptr;

    use windows_sys::core::{PCWSTR, w};

    use super::{
        GENERIC_ALL, HKEY_LOCAL_MACHINE, MAX_PROXY_VALUE_BYTES, ProxyEnable, RegistryKey,
        WellKnownSid, WinLocalSystemSid, default_dacl_grants, file_id, file_index, filtered_token,
        is_same_file, long_path, numbered, onedrive_account_folders, reparse_tag, resource_bytes,
        set_short_name, short_path, token_user_bytes, update_resources, wide_string, windows_build,
    };

    /// The first build of Windows 10, the oldest system the launcher runs on.
    const FIRST_WINDOWS_10_BUILD: u32 = 10_240;

    /// The machine's key that names the system's version, with a number and a string among its
    /// values on every Windows 10 and later, which the tests only read.
    const VERSION_KEY: PCWSTR = w!(r"Software\Microsoft\Windows NT\CurrentVersion");

    /// The version key's number value naming the system's major version, 10 or more.
    const MAJOR_VERSION_VALUE: PCWSTR = w!("CurrentMajorVersionNumber");

    /// The version key's string value naming the system's build, in decimal.
    const BUILD_VALUE: PCWSTR = w!("CurrentBuild");

    /// A value no key of the system holds.
    const MISSING_VALUE: PCWSTR = w!("GauntletDarkLegacyLauncherNoSuchValue");

    /// A key the system does not have.
    const MISSING_KEY: PCWSTR = w!(r"Software\GauntletDarkLegacyLauncherNoSuchKey");

    /// IO_REPARSE_TAG_MOUNT_POINT: the reparse tag of a junction.
    const JUNCTION_TAG: u32 = 0xA000_0003;

    #[test]
    fn resource_numbers_go_as_addresses() {
        // MAKEINTRESOURCE's form: the number itself, as the address of nothing.
        for number in [0, 1, 3, 14, 24, 1033, u16::MAX] {
            assert_eq!(numbered(number).addr(), usize::from(number));
        }
    }

    #[test]
    fn a_file_that_is_no_program_is_refused_and_left_as_it_was() {
        // Neither an update nor a read takes a file that is no program; the system's own error
        // comes back, and the file is as it was.
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("invalid.exe");
        fs::write(&file, b"not a PE").unwrap();
        let error = update_resources(&file, &[]).unwrap_err();
        assert!(error.raw_os_error().is_some(), "{error:?}");
        let error = resource_bytes(&file, 3, 1, 0).unwrap_err();
        assert!(error.raw_os_error().is_some(), "{error:?}");
        assert_eq!(fs::read(&file).unwrap(), b"not a PE");
        // Nor does a file that is not there.
        let missing = dir.path().join("missing.exe");
        assert!(update_resources(&missing, &[]).is_err());
        assert!(!missing.exists());
    }

    #[test]
    fn the_build_is_the_systems_own() {
        // The system's own build, whatever manifest the test program carries.
        let build = windows_build();
        assert!(build >= FIRST_WINDOWS_10_BUILD, "{build}");
    }

    #[test]
    fn a_reparse_tag_is_read_from_the_entry_itself() {
        let dir = tempfile::tempdir().unwrap();
        let real = dir.path().join("real");
        fs::create_dir(&real).unwrap();
        let file = dir.path().join("file.txt");
        fs::write(&file, b"kept").unwrap();
        let junction = dir.path().join("junction");
        gdl_fixtures::make_junction(&junction, &real).unwrap();

        // A junction is a reparse point, read as itself, not as the folder it leads to.
        assert_eq!(reparse_tag(&junction).unwrap(), Some(JUNCTION_TAG));
        assert_eq!(reparse_tag(&real).unwrap(), None);
        assert_eq!(reparse_tag(&file).unwrap(), None);
        // A path past MAX_PATH is read too.
        let long = gdl_fixtures::long_root(&dir.path().join("long"), 280).unwrap();
        assert_eq!(reparse_tag(&long).unwrap(), None);
        // Nothing there is the system's own error, and nothing was opened to write.
        let error = reparse_tag(&dir.path().join("missing")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound, "{error}");
        assert_eq!(fs::read(&file).unwrap(), b"kept");
        assert!(fs::read_dir(&real).unwrap().next().is_none());
    }

    #[test]
    fn an_open_file_is_told_from_another_at_its_name() {
        let dir = tempfile::tempdir().unwrap();
        let name = dir.path().join("lock");
        fs::write(&name, b"").unwrap();
        let file = fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(&name)
            .unwrap();
        let other = dir.path().join("other");
        fs::write(&other, b"").unwrap();
        // The file at its own name, by either identity, and not at another file's name.
        assert!(is_same_file(&file, &name).unwrap());
        assert!(!is_same_file(&file, &other).unwrap());
        let reopened = fs::File::open(&name).unwrap();
        assert_eq!(file_id(&file).unwrap(), file_id(&reopened).unwrap());
        assert_eq!(file_index(&file).unwrap(), file_index(&reopened).unwrap());
        let elsewhere = fs::File::open(&other).unwrap();
        assert_ne!(file_id(&file).unwrap(), file_id(&elsewhere).unwrap());
        assert_ne!(file_index(&file).unwrap(), file_index(&elsewhere).unwrap());
        drop((reopened, elsewhere));

        // Moved away while open, and another file made at its name: the name leads elsewhere.
        fs::rename(&name, dir.path().join("moved")).unwrap();
        fs::write(&name, b"another").unwrap();
        assert!(!is_same_file(&file, &name).unwrap());
        // A junction at the name is itself, not the folder it leads to, nor the file.
        let real = dir.path().join("real");
        fs::create_dir(&real).unwrap();
        let junction = dir.path().join("junction");
        gdl_fixtures::make_junction(&junction, &real).unwrap();
        let folder = fs::OpenOptions::new()
            .access_mode(0)
            .custom_flags(super::FILE_FLAG_BACKUP_SEMANTICS)
            .open(&real)
            .unwrap();
        assert!(!is_same_file(&folder, &junction).unwrap());
        assert!(!is_same_file(&file, &junction).unwrap());
        // Nothing at the name is the system's own error.
        let error = is_same_file(&file, &dir.path().join("missing")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound, "{error}");
    }

    #[test]
    fn a_path_is_given_by_its_full_names() {
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("a folder of a long name");
        fs::create_dir(&folder).unwrap();
        // In its verbatim form, its own name kept.
        let full = long_path(&folder).unwrap();
        assert!(full.as_os_str().to_string_lossy().starts_with(r"\\?\"));
        assert_eq!(full.file_name(), folder.file_name());
        // A short alias gives way to the name it stands for, where this process may give one.
        if set_short_name(&folder, "AFOLDE~1").is_ok() {
            assert_eq!(
                short_path(&folder).unwrap().file_name().unwrap(),
                "AFOLDE~1"
            );
            let full = long_path(&dir.path().join("AFOLDE~1")).unwrap();
            assert_eq!(full.file_name(), folder.file_name());
        } else {
            eprintln!("skipped the alias: this process may give no folder one");
        }
        // A path past MAX_PATH is given too.
        let long = gdl_fixtures::long_root(&dir.path().join("long"), 280).unwrap();
        assert_eq!(long_path(&long).unwrap().file_name(), long.file_name());
        // Nothing there is the system's own error.
        let error = long_path(&dir.path().join("missing")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound, "{error}");
    }

    #[test]
    fn the_onedrive_accounts_name_folders() {
        // The registry is only read, and an account that names no folder gives none.
        for folder in onedrive_account_folders() {
            assert!(!folder.as_os_str().is_empty());
        }
    }

    #[test]
    fn a_switch_is_read_as_a_number_or_as_text() {
        let version = RegistryKey::open(HKEY_LOCAL_MACHINE, VERSION_KEY).unwrap();
        // A number value is read as the number.
        match version.switch(MAJOR_VERSION_VALUE) {
            Some(ProxyEnable::Number(major)) => assert!(major >= 10, "{major}"),
            Some(ProxyEnable::Text(_)) => panic!("the major version was read as text"),
            None => panic!("the major version was not read"),
        }
        // A string value, which no number read takes, is read as its text, without its NUL.
        assert_eq!(version.number(BUILD_VALUE), None);
        match version.switch(BUILD_VALUE) {
            Some(ProxyEnable::Text(build)) => {
                let build: u32 = build.parse().unwrap();
                assert!(build >= FIRST_WINDOWS_10_BUILD, "{build}");
            }
            Some(ProxyEnable::Number(_)) => panic!("the build was read as a number"),
            None => panic!("the build was not read"),
        }
        // A value the key lacks is neither.
        assert!(version.switch(MISSING_VALUE).is_none());
        assert_eq!(version.number(MISSING_VALUE), None);
    }

    #[test]
    fn a_string_is_read_within_its_bound() {
        let version = RegistryKey::open(HKEY_LOCAL_MACHINE, VERSION_KEY).unwrap();
        let build = version
            .text(ptr::null(), BUILD_VALUE, MAX_PROXY_VALUE_BYTES)
            .unwrap();
        assert!(!build.is_empty());
        // A bound that holds less than the string and its NUL reads nothing.
        let short = u32::try_from(build.len() * 2).unwrap();
        assert_eq!(version.text(ptr::null(), BUILD_VALUE, short), None);
        // A number is no string, and a value the key lacks is none.
        assert_eq!(
            version.text(ptr::null(), MAJOR_VERSION_VALUE, MAX_PROXY_VALUE_BYTES),
            None
        );
        assert_eq!(
            version.text(ptr::null(), MISSING_VALUE, MAX_PROXY_VALUE_BYTES),
            None
        );
        // A key the system lacks is its own error.
        let Err(error) = RegistryKey::open(HKEY_LOCAL_MACHINE, MISSING_KEY) else {
            panic!("a key the system lacks was opened");
        };
        assert_eq!(error.kind(), io::ErrorKind::NotFound, "{error}");
    }

    #[test]
    fn a_wide_string_ends_at_its_nul() {
        let text: Vec<u16> = "C:\\Users\0after".encode_utf16().collect();
        // SAFETY: the string holds a NUL, before which its units are read.
        let units = unsafe { wide_string(text.as_ptr()) };
        assert_eq!(String::from_utf16(units).unwrap(), "C:\\Users");
        let empty = [0_u16];
        // SAFETY: the same.
        assert!(unsafe { wide_string(empty.as_ptr()) }.is_empty());
    }

    #[test]
    fn a_filtered_token_grants_what_its_program_makes_to_its_user_and_system() {
        // As a standard user's token does, whatever this process's own default DACL grants: an
        // administrator's may grant only Administrators, which the filtered token holds only to
        // deny, and SYSTEM.
        let token = filtered_token().unwrap();
        let system = WellKnownSid::of(WinLocalSystemSid).unwrap();
        assert_eq!(
            default_dacl_grants(&token).unwrap(),
            [
                (token_user_bytes(&token).unwrap(), GENERIC_ALL),
                (system.bytes(), GENERIC_ALL),
            ]
        );
    }
}
