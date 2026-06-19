// SPDX-License-Identifier: GPL-3.0-or-later

//! Prints source info for a TBC capture: `video_info <path.tbc>`

use chromadec::Video;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = std::env::args()
        .nth(1)
        .ok_or("usage: video_info <path.tbc>")?;
    let video = Video::open_composite(&path, None, None)?;
    let info = video.info()?;
    println!("{path}: {info:#?}");
    println!("libchromadec {}", chromadec::version_string());
    Ok(())
}
