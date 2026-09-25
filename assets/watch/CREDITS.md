# Watch face photos: credits

These photos sit behind the dial on the watch board. Each one comes as two files:

- `<name>.png` is the 240x280 RGB master, composed from the original.
- `<name>.bin` is that master converted to raw RGB565: little-endian uint16 per pixel, row-major, top row first, 134,400 bytes. The firmware embeds this file.

The masters have no dimming and no corner rounding baked in. The firmware does both. To regenerate a `.bin`:

    python3 tools/gen_image.py assets/watch/<name>.png assets/watch/<name>.bin

The conversion applies a 4x4 ordered dither, and any pixel with all three channels at 4 or below becomes pure black. See the docstring in `tools/gen_image.py`.

All three images are NASA works and public domain in the United States. NASA's media guidelines (https://www.nasa.gov/nasa-brand-center/images-and-media/) ask that "NASA should be acknowledged as the source of the material". They also say the material must not be used in a way that implies NASA endorses anything. Wherever the watch or its source is shown, keep the credit lines below with their wording unchanged.

---

## earth: The Blue Marble

- **Title:** "The Blue Marble", Apollo 17, NASA photo AS17-148-22727 (also GPN-2000-001138), 7 Dec 1972
- **Subject:** Earth from space, the full disc. Africa, Arabia, Madagascar and the Antarctic ice cap sit under swirling white cloud, with black space around the globe.
- **Credit:** NASA / Apollo 17 crew (Harrison Schmitt or Ron Evans), "The Blue Marble", AS17-148-22727, 7 Dec 1972. Public domain.
- **Licence:** Public domain, as a NASA work. The Wikimedia Commons file page says: "This file is in the public domain in the United States because it was solely created by NASA." NASA's own catalogue page (https://nssdc.gsfc.nasa.gov/imgcat/html/object_page/a17_h_148_22727.html) gives the credit line "(Apollo 17, AS17-148-22727)".
- **Source:** https://commons.wikimedia.org/wiki/File:The_Earth_seen_from_Apollo_17.jpg
- **Downloaded from:** https://upload.wikimedia.org/wikipedia/commons/9/97/The_Earth_seen_from_Apollo_17.jpg (3000x3002 JPEG)
- **What was done:**
  - Scaled so the disc is 216 px across (factor 216/2680), with one LANCZOS resize.
  - Padded with black onto a 240x280 canvas, not cropped, so the disc is centred at (120, 140). The original is too short for a 280 px tall crop at this scale.
  - Converted to RGB565 with `tools/gen_image.py`: ordered dither, and near-black sky noise cleared to pure black.

## moon: the full Moon

- **Title:** Moon Phase and Libration, 2026 (SVS ID 5587), frame 8571: the full Moon, 24 Dec 2026 02:00 UT
- **Subject:** The full Moon, the whole disc with north up, on pure black. This is a photorealistic NASA Scientific Visualization Studio render built from Lunar Reconnaissance Orbiter data (the LOLA elevation map and the LROC WAC colour mosaic). It is not a single camera photograph.
- **Credit:** Moon: NASA Scientific Visualization Studio (visualizer Ernie Wright, USRA), "Moon Phase and Libration, 2026" (SVS 5587), frame 8571. Rendered from LRO LOLA and LROC WAC data. Public domain.
- **Licence:** Public domain. https://svs.gsfc.nasa.gov/help/ says: "All of our content is in the public domain (unless otherwise noted), meaning that it is free to download, use, and redistribute for whatever purposes you see fit." The source page lists no exception. Its credits are: NASA Scientific Visualization Studio; Visualizer Ernie Wright (USRA); Planetary scientist Noah Petro (NASA/GSFC); Producer James Tralie (eMITS).
- **Source:** https://svs.gsfc.nasa.gov/5587/
- **Downloaded from:** https://svs.gsfc.nasa.gov/vis/a000000/a005500/a005587/frames/5760x3240_16x9_30p/plain/moon.8571.tif (5760x3240 RGBA TIFF)
- **What was done:**
  - Composited onto opaque black.
  - Cropped to a box centred on the disc and resized once with LANCZOS (about 14.3:1) to 240x280. The disc is 218 px across, centred at (120, 140).
  - The black above and below the disc is zero padding.
  - Converted to RGB565 with `tools/gen_image.py`.

## space: Cosmic Cliffs

- **Title:** "Cosmic Cliffs" in the Carina Nebula (NIRCam image), James Webb Space Telescope, released 12 July 2022
- **Subject:** Deep space, the "Cosmic Cliffs" of NGC 3324 in the Carina Nebula. A blue starfield fills the top and the orange-gold cliff face of the nebula fills the bottom.
- **Credit:** "Cosmic Cliffs" in the Carina Nebula (NIRCam). Image: NASA, ESA, CSA, STScI
- **Licence:** Public domain as a NASA-produced work, and this copy came from NASA. The NASA asset page gives the credit as "Credit Image: NASA, ESA, CSA, STScI". ESA/Webb also publishes the same picture as weic2205a (https://esawebb.org/images/weic2205a/) under CC BY 4.0. That licence requires the full credit to be shown clearly with its wording unaltered. To satisfy both, always show the full credit "NASA, ESA, CSA, STScI" and never shorten it.
- **Source:** https://science.nasa.gov/asset/webb/cosmic-cliffs-in-the-carina-nebula-nircam-image/
- **Downloaded from:** https://assets.science.nasa.gov/content/dam/science/missions/webb/science/2022/07/STScI-01GA6KKWG229B16K4Q38CH3BXS.png ("Full Res (For Display)", 14575x8441 PNG)
- **What was done:**
  - Cropped to the box (4582, 728, 9704, 6704), which is 5122x5976, the watch's 6:7 aspect.
  - Resized once with LANCZOS to 240x280, with no sharpening and no colour changes.
  - Removed the embedded sRGB ICC profile. The pixels did not change.
  - Converted to RGB565 with `tools/gen_image.py`.
