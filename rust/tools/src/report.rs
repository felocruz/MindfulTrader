//! Archived tool output -- the Rust counterpart of `tools/ToolProgressLogger.h` (standing
//! directive 2026-09-03: every long-running tool keeps its full output). Every line goes to stdout
//! and to `tools/output/<tool>_<stamp>.txt`; `finish` appends a PENDING REVIEW row to the
//! git-tracked `tools/RECALIBRATION_LEDGER.md` (directive 2026-09-04).

use std::fs::{File, OpenOptions};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Debug)]
pub struct Report {
    repo_root: PathBuf,
    tool: String,
    scope: String,
    archive_rel: String,
    file: BufWriter<File>,
    started: String,
}

/// Local time as `date` formats it (no time-zone crate needed for a stamp).
fn local_time(format: &str) -> String {
    Command::new("date")
        .arg(format!("+{format}"))
        .output()
        .ok()
        .and_then(|o| String::from_utf8(o.stdout).ok())
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "unknown-time".to_string())
}

impl Report {
    /// `repo_root` is the MindfulTrader checkout; `scope` describes the run for the ledger row.
    pub fn open(repo_root: &Path, tool: &str, scope: &str) -> std::io::Result<Self> {
        let stamp = local_time("%Y%m%d_%H%M%S");
        let archive_rel = format!("tools/output/{tool}_{stamp}.txt");
        let path = repo_root.join(&archive_rel);
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let file = BufWriter::new(File::create(&path)?);
        Ok(Self { repo_root: repo_root.to_path_buf(), tool: tool.into(), scope: scope.into(), archive_rel, file, started: local_time("%Y-%m-%d %H:%M") })
    }

    pub fn archive_path(&self) -> &str {
        &self.archive_rel
    }

    /// One line to stdout and the archive, flushed (a `tail -f` on either follows live).
    pub fn line(&mut self, text: impl AsRef<str>) {
        let text = text.as_ref();
        println!("{text}");
        let _ = std::io::stdout().flush();
        let _ = writeln!(self.file, "{text}");
        let _ = self.file.flush();
    }

    /// Closes the archive and appends the ledger row.
    pub fn finish(mut self) -> std::io::Result<()> {
        self.file.flush()?;
        let ledger = self.repo_root.join("tools/RECALIBRATION_LEDGER.md");
        let mut f = OpenOptions::new().append(true).open(&ledger)?;
        writeln!(f, "| {} | {} | {} | {} | PENDING REVIEW |", self.started, self.tool, self.scope.replace('|', "/"), self.archive_rel)
    }
}
