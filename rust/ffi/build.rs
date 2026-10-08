// Generates include/generated/mts_core.h -- the C++ view of this crate's C-ABI -- on every build.
// Mirrors Atratus's atratus_sensor/build.rs pattern.
//
// Path note: today (pre-merge) this crate lives at rust/ffi/, two levels below the MindfulTrader
// repo root, so the header lands at ../../include/generated/ from here. Post-merge, once this repo's
// C++ moves into cpp/ and rust/ becomes a top-level sibling (plan §1), this path changes to
// ../../cpp/include/generated/ -- update in the same commit as that move, not before.
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-changed=build.rs");

    let crate_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let header = crate_dir.join("../../include/generated/mts_core.h");
    let config = cbindgen::Config::from_file(crate_dir.join("cbindgen.toml")).unwrap();
    cbindgen::Builder::new()
        .with_src(crate_dir.join("src/lib.rs"))
        .with_config(config)
        .generate()
        .expect("Unable to generate mts_core.h")
        .write_to_file(header);
}
