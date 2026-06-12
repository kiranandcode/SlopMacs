 - want to setup an electron wrapper around this. it should be a macos .app that I can just double click and it handles everything

preemptive-threads follow-ups:

 - FOREGROUND-PRIORITY SCHEDULER: detached background commands currently get
   equal time slices with the foreground executor; under two CPU-bound
   background commands, typing latency was ~2s. Give the foreground executor
   priority (e.g. background threads get longer slices / yield more often, or
   foreground gets first claim on the lock after a yield).
 - way to kill a detached background command (thread-signal UX; maybe double C-g)
 - process filters/sentinels/timers through the executor machinery
 - dogfood magit on a real repo; decide same-buffer collision policy
 - pre-existing: synthetic font_metrics/resize from a client spins Emacs at
   99% CPU (resize loop; reproduces on stock build too)

things I want to fix:

 - cursor clicking is slightly off.
 - underlining words? (currently writing in scratch buffer, and all my words are being underlined for some reason)
 - image display (about emacs image displays but the text is not pushed beneath it)
 - webkit components integration - would love to have an emacs api to allow me to embed javascript widgets into an emacs buffer
   (ideally in a backwards compatible way, so vanilla emacs just sees the javascript)
 - keyboard bindings (in the electron app; in firefox ofc not possible)
 - some commands don 't work the same in this build as they do on my default emacs... why??
 - some rendering glitches, when the buffer gets too long then the text disappears
