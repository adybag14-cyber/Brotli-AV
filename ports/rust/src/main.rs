use bav::{compress, decompress, Method};
use clap::{Parser, Subcommand};
use std::fs;
use std::path::PathBuf;
use std::process::ExitCode;

#[derive(Parser)]
#[command(name = "bav-rust", about = "BAV1 research compressor (Rust)")]
struct Cli {
    #[command(subcommand)]
    cmd: Commands,
}

#[derive(Subcommand)]
enum Commands {
    Compress {
        input: PathBuf,
        #[arg(short, long)]
        output: Option<PathBuf>,
        /// auto|store|deflate|lzma|zstd|brotli|research (default: auto — full research path)
        #[arg(short, long, default_value = "auto")]
        method: String,
    },
    Decompress {
        input: PathBuf,
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
    Version,
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    match cli.cmd {
        Commands::Version => {
            println!("bav-rust {}", env!("CARGO_PKG_VERSION"));
            ExitCode::SUCCESS
        }
        Commands::Compress {
            input,
            output,
            method,
        } => {
            let method = match Method::parse(&method) {
                Ok(m) => m,
                Err(e) => {
                    eprintln!("{e}");
                    return ExitCode::from(2);
                }
            };
            let data = match fs::read(&input) {
                Ok(d) => d,
                Err(e) => {
                    eprintln!("read {}: {e}", input.display());
                    return ExitCode::FAILURE;
                }
            };
            let frame = match compress(&data, method) {
                Ok(f) => f,
                Err(e) => {
                    eprintln!("compress: {e}");
                    return ExitCode::FAILURE;
                }
            };
            let out = output.unwrap_or_else(|| {
                PathBuf::from(format!("{}.bav", input.display()))
            });
            if let Err(e) = fs::write(&out, &frame) {
                eprintln!("write {}: {e}", out.display());
                return ExitCode::FAILURE;
            }
            println!(
                "compressed {} -> {} bytes ({})",
                data.len(),
                frame.len(),
                out.display()
            );
            ExitCode::SUCCESS
        }
        Commands::Decompress { input, output } => {
            let frame = match fs::read(&input) {
                Ok(d) => d,
                Err(e) => {
                    eprintln!("read {}: {e}", input.display());
                    return ExitCode::FAILURE;
                }
            };
            let data = match decompress(&frame) {
                Ok(d) => d,
                Err(e) => {
                    eprintln!("decompress: {e}");
                    return ExitCode::FAILURE;
                }
            };
            let out = output.unwrap_or_else(|| {
                let s = input.to_string_lossy();
                if s.ends_with(".bav") {
                    PathBuf::from(&s[..s.len() - 4])
                } else {
                    PathBuf::from(format!("{}.out", s))
                }
            });
            if let Err(e) = fs::write(&out, &data) {
                eprintln!("write {}: {e}", out.display());
                return ExitCode::FAILURE;
            }
            println!(
                "decompressed {} -> {} bytes ({})",
                frame.len(),
                data.len(),
                out.display()
            );
            ExitCode::SUCCESS
        }
    }
}
