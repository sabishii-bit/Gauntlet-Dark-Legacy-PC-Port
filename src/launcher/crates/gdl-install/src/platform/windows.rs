//! The Windows calls: setting resources into a program with BeginUpdateResourceW, UpdateResourceW
//! and EndUpdateResourceW, reading one back as data for the tests, and telling the shell with
//! SHChangeNotify that a file it shows has changed.

use std::io;
use std::marker::PhantomData;
use std::mem::ManuallyDrop;
use std::os::windows::ffi::OsStrExt;
use std::path::Path;
use std::ptr;

use gdl_files::{VerbatimPath, strip_verbatim};
use windows_sys::Win32::Foundation::{FALSE, HANDLE, TRUE};
use windows_sys::Win32::System::LibraryLoader::{
    BeginUpdateResourceW, EndUpdateResourceW, UpdateResourceW,
};
use windows_sys::Win32::UI::Shell::{SHCNE_UPDATEITEM, SHCNF_PATHW, SHChangeNotify};
use windows_sys::core::PCWSTR;

#[cfg(test)]
use windows_sys::Win32::Foundation::{
    ERROR_RESOURCE_LANG_NOT_FOUND, ERROR_RESOURCE_NAME_NOT_FOUND, ERROR_RESOURCE_TYPE_NOT_FOUND,
    FreeLibrary, HMODULE, WIN32_ERROR,
};
#[cfg(test)]
use windows_sys::Win32::System::LibraryLoader::{
    FindResourceExW, LOAD_LIBRARY_AS_DATAFILE, LOAD_LIBRARY_AS_IMAGE_RESOURCE, LoadLibraryExW,
    LoadResource, LockResource, SizeofResource,
};

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
    /// Loads the program at `executable` as an image of data, whose code never runs. It goes to
    /// the call in its verbatim form, so that it is not held to MAX_PATH.
    fn load(executable: &Path) -> io::Result<DataModule> {
        let path = VerbatimPath::new(executable)?;
        // SAFETY: the path is a NUL-terminated UTF-16 string that `path` owns and keeps alive
        // until the call returns, which only reads it; the call takes no file handle, which is
        // null as it asks; and the flags map the file as data, so nothing of it runs.
        let module = unsafe {
            LoadLibraryExW(
                path.as_ptr(),
                ptr::null_mut(),
                LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE,
            )
        };
        if module.is_null() {
            Err(io::Error::last_os_error())
        } else {
            Ok(DataModule(module))
        }
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

#[cfg(test)]
mod tests {
    use std::fs;

    use super::{numbered, resource_bytes, update_resources};

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
}
