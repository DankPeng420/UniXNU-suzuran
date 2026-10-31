# NativeDisplay

The iOS display for the Lumia 950 XL: IOSurface (`IOCoreSurfaceRoot`) and IOMobileFramebuffer (`AppleCLCD`)
implemented at the user-client level of the iOS 16.7.16 frameworks, scanning out through the firmware's GOP
framebuffer (the memory the panel already shows; the kernel's verbose console draws into it too).

Ported from ios-sweet (MIT, see the source headers). CoreAnimation renders on the CPU into IOSurfaces;
`SwapEnd` records the frame and a 60 Hz timer (vsync notifications) copies the latest one into the panel at
`lumiafb_hz` (default 10 Hz). The display is presented as the iPhone X canvas, 1125x2436 @3x, centred in the
1440x2560 panel (`lumiafb_native=1` presents the panel size). The text console stops drawing from the first
presented frame (`lumiafb_console=1` keeps it; panics always draw).

Needs the ADT's `/arm-io/disp0` (device_type `display-subsystem`, canvas size) and the iPhone identity
(`board.toml` `iphone_identity`). Personalities are published at runtime by the zzv overlay patch.
