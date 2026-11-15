# The OpenGL ES route for iOS's UI (experimental)

Status 2026-11-15: paused. The stable image (`suzuran/README.md`, `full33`) renders in software.

## How it works
- The kernel puts `DYLD_INSERT_LIBRARIES=/usr/lib/libsuzuran.inject.dylib` into the environment of the
  processes named by boot-arg `dyldinsert=` (launchd strips DYLD_* from job definitions), and answers AMFI's
  dyld policy query with "allow" under `dyldallow=1` (`unixnu-overlay/patches/xnu-8796/zzt2-dyld-allow.patch`,
  `zzt3-dyld-insert.patch`).
- `userland/inject`: with `suzurangl=1`, in backboardd, loads `/usr/lib/libsuzuran.gl.dylib`.
- `userland/eagl`: HakiOS's eagl.c (swizzles 8 EAGLContext methods, fills OpenGLES.framework's dispatch
  table) on our OSMesa/freedreno; adds 'b3a8' (plane 0 as BGRA) and 'w30r' (10:10:10:2) render targets and
  packs padded rows itself (Mesa ignores GL_PACK_ROW_LENGTH).
- `drivers/NativeAdreno430`: with `suzurangl=1` a buffer pool of 192 MB taken at start (late in boot the
  kernel rarely has 4 MB contiguous); selector 0x7e imports an IOSurface whose memory came from the pool
  as a GPU buffer (HakiOS's HAKIOS_GPU_SURFACE); 0x7c / 0x7b name the panel buffer to draw onto and mark
  it drawn.
- `drivers/NativeDisplay`: backboardd's big screen surfaces come from the GPU pool (rows rounded to 128
  bytes), so the GPU draws into them; a plane's bytes per row are at +0x10 of the plane record;
  WrapClientImage makes surfaces of client pictures (wired read-only); the page flip finds the MDSS
  source pipe the bootloader left scanning out the panel (by SSPP_SRC0_ADDR; pipe 0x17000, ctl 0x2000 on
  this phone) and points it at a second panel buffer from the pool (`lumiafb_flip=0` disables).

## Results
- Lock screen, home screen and Settings render correctly on the GPU; backboardd stable.
- Frame cost 20–45 ms, most of it waiting for the GPU (before zero copy: 250–450 ms copying frames out).
- Open: (1) frames reach the panel with some layers missing (flicker; the last fix, flipping at the swap to
  the buffer holding the swapped surface, was built but not tested); (2) the home screen's app icons never
  reach the GL layer (Settings' icons do); (3) without the GPU writing the panel (`glpanel=0`) the CPU copy
  out of write-combined GPU memory costs 50–110 ms a frame.

## Boot-args
`dyldallow=1 dyldinsert=backboardd suzurangl=1` on top of the stable ones; diagnostics `glpanel=0`,
`lumiafb_flip=0`, `adrenopool=MB`, `glcensus=1`, `gltrace_at=SECONDS gltrace_frames=N gltrace_place=X0,Y0,X1,Y1`
(see the comment at the top of `userland/eagl/eagl.c`).
