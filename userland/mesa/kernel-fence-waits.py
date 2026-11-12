#!/usr/bin/env python3
"""Make freedreno ask the kernel whether a submission is done instead of trusting its own fence.

freedreno writes its own fence (pipe->control->fence) with CP_EVENT_WRITE CACHE_FLUSH_TS at the end of each batch,
and fd_pipe_wait_timeout returns at once when that fence has passed, without asking the kernel. On the A4xx nothing
waits for idle before that event, so the fence can land while a draw is still in the pipeline (boot 25: the clear
without the triangle; boot 32: 101 submissions, no WAIT_FENCE). Without the early return, every wait goes to
MSM_WAIT_FENCE, whose fence NativeAdreno430 writes after HLSQ_FLUSH + CP_WAIT_FOR_IDLE.

Usage: kernel-fence-waits.py MESA_SOURCE_DIR. Fails, printing the function, when the code is not as expected.
"""
import re, sys, pathlib

path = pathlib.Path(sys.argv[1]) / "src/freedreno/drm/freedreno_pipe.c"
text = path.read_text()
start = text.find("\nfd_pipe_wait_timeout(")
if start < 0:
    sys.exit(f"{path}: no fd_pipe_wait_timeout")
end = text.find("\n}\n", start)
body = text[start:end]
# the early return on the userspace fence: if (!fd_fence_after(fence->ufence, pipe->control->fence)) return 0;
pattern = re.compile(r"\n[ \t]*if \(!fd_fence_after\(fence->ufence, pipe->control->fence\)\)\s*\n?\s*return 0;")
matches = pattern.findall(body)
if len(matches) != 1:
    print(body)
    sys.exit(f"{path}: expected one early return on pipe->control->fence in fd_pipe_wait_timeout, found {len(matches)}")
# Mesa's fence passed: the kernel's decides (a zero timeout still only asks)
new_body = pattern.sub("\n   if (!fd_fence_after(fence->ufence, pipe->control->fence))\n"
                       "      return pipe->funcs->wait(pipe, fence, timeout); /* suzuran: kernel-fence-waits.py */", body)
path.write_text(text[:start] + new_body + text[end:])
print(f"{path}: fd_pipe_wait_timeout asks the kernel even when its own fence has passed")
