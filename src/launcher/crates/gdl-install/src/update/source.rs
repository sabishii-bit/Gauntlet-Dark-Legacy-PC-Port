//! Where an update takes the new release from: the launcher file that runs, which carries the
//! release it was built with and is copied whole as the new launcher copy. Its sections are read
//! against their table before anything of the runtime is trusted, and its runtime is held to the
//! one this launcher installs.

use std::fs::File;

use gdl_files::Job;
use gdl_release::Version;

use super::{UpdateError, payload_error, sections_error};
use crate::payload::Payload;
use crate::sections::{SectionView, Sections};

/// Where an update takes the new game and launcher from.
pub enum UpdateSource<'a> {
    /// This launcher file, through the handle it opened as it started, which reads the file that
    /// started even once another has taken its path.
    Bundled(&'a mut Sections<File>),
}

impl UpdateSource<'_> {
    /// The launcher file the update reads the new release from and copies as the new launcher
    /// copy.
    pub(super) fn launcher_file(&mut self) -> &mut Sections<File> {
        match self {
            UpdateSource::Bundled(sections) => sections,
        }
    }
}

/// The runtime the launcher file `sections` carries, once every section is read against its
/// table, at most a block at a time, with `job`'s cancel looked at before each and its progress
/// told after; held, as a plan holds it, to the one a launcher of version `running` installs, of
/// this program's platform and of that version as text.
pub(super) fn verified_runtime<'s>(
    sections: &'s mut Sections<File>,
    running: &Version,
    job: &Job<'_>,
) -> Result<Payload<SectionView<'s, File>>, UpdateError> {
    sections.verify(job).map_err(sections_error)?;
    let payload = sections.runtime().map_err(payload_error)?;
    payload
        .for_launcher(&running.to_string())
        .map_err(payload_error)?;
    Ok(payload)
}
