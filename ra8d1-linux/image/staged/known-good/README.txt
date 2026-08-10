rvlinux-tcp-push.elf: current rvlinux build (Aug 7 22:37, 19,587,080 B) with
the loader reliability fixes and diagnostics (BLKCRC, ERASE INCOMPLETE,
verified-on-attempt). This is the one to flash for recovery.

rvlinux-tcp-push-OLD-no-diag.elf: the earlier archive (19,533,024 B). It
PREDATES the loader fixes: on 2026-08-08 it produced 4 consecutive rootfs
push failures that the current build did not. Kept only for provenance.
Lesson: refresh the recovery archive whenever the app gains fixes; a stale
"known-good" quietly reintroduces every bug fixed since it was taken.
