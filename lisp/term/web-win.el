;;; web-win.el --- set up windowing for web display backend -*- lexical-binding: t -*-

;; Copyright (C) 2026 Free Software Foundation, Inc.

;; This file is part of GNU Emacs.

;; GNU Emacs is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; GNU Emacs is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.

;;; Commentary:

;; Support for using Emacs via a web browser (WebSocket + Canvas).

;;; Code:

(eval-when-compile (require 'cl-lib))
(unless (featurep 'web)
  (error "%s: Loading web-win without having web display support"
         invocation-name))

(require 'frame)
(require 'mouse)
(require 'scroll-bar)
(require 'menu-bar)
(require 'fontset)
(eval-when-compile (require 'mwheel))

(add-to-list 'display-format-alist '(".*" . web))

;; Disable JIT native compilation in the web backend.
;; The toolchain (gcc, as, ld) may not be on PATH when Emacs is spawned
;; from an Electron app bundle.  Pre-compiled .eln files still work.
(setq native-comp-jit-compilation nil)

;; Use text/symbol icons instead of images.  The web backend renders
;; images by sending pixel data to the browser, which works but makes
;; the image cache extremely expensive for mode-line icons (tab-bar
;; close buttons, etc.).  SVG icon lookup causes O(n^2) cache scans.
(setq icon-preference '(symbol text))

;; Shorten image cache eviction to prevent unbounded growth.
;; Face changes (custom-set-faces) create stale image cache entries
;; with old face parameters.  Short eviction keeps bucket chains small.
(setq image-cache-eviction-delay 30)

;; Raise GC threshold to avoid frequent garbage collection during
;; complex mode-line evaluation (doom-modeline, all-the-icons, etc.).
;; Default 800KB is too low for web display with rich mode lines.
(setq gc-cons-threshold (* 50 1024 1024))

;;;; Command line argument handling.

(defvar x-invocation-args)
(defvar x-command-line-resources)

(cl-defmethod window-system-initialization (&context (window-system web)
                                                     &optional display)
  "Set up the window system.  WINDOW-SYSTEM must be WEB.
DISPLAY may be set to the name of a display that will be initialized."
  (create-default-fontset)
  (x-open-connection (or display "web") x-command-line-resources t))

(cl-defmethod frame-creation-function (params &context (window-system web))
  (x-create-frame-with-faces params))

(cl-defmethod handle-args-function (args &context (window-system web))
  (x-handle-args args))

(defun web--snap-vscroll (win)
  "Snap WIN's vscroll to 0 at the nearest line boundary.
A precision scroll leaves a fractional vscroll, which redisplay resets
to 0 on the *next* command (xdisp.c) — seen as the whole buffer jumping
a few pixels when you next move the cursor.  Doing the snap here, as
part of the scroll gesture, pre-empts that (≤ half a line, so barely
visible)."
  (let ((vs (window-vscroll win t))
        (lh (max 1 (frame-char-height))))
    (when (and (integerp vs) (> vs 0))
      (if (< vs (/ lh 2))
          ;; Most of the top line is visible: just drop the vscroll
          ;; (content settles down by VS px).
          (set-window-vscroll win 0 t)
        ;; Most of it is scrolled off: advance one line (content settles
        ;; up by LH-VS px).
        (set-window-start
         win (save-excursion (goto-char (window-start win))
                             (vertical-motion 1) (point))
         t)
        (set-window-vscroll win 0 t)))))

(defcustom web-scroll-page-lerp 0.20
  "Fraction of the remaining distance each C-v / M-v scroll step covers.
Lower = a longer, glidier deceleration; higher = snappier."
  :type 'number :group 'web-fx)

(defcustom web-scroll-page-tick 0.006
  "Seconds between C-v / M-v scroll steps."
  :type 'number :group 'web-fx)

(defun web--settle (win _advance-test lh)
  "Soft-land WIN's leftover vscroll onto the nearest line boundary.
A lerp-to-zero so the scroll *decelerates* into rest instead of snapping
abruptly.  Direction is derived from the current vscroll."
  (let ((vs (window-vscroll win t)))
    (when (and (integerp vs) (> vs 0))
      (let* ((advance (>= vs (/ lh 2)))
             (rem (if advance (- lh vs) vs)))
        (catch 'web--settled
          (while (> rem 1)
            (let ((d (max 1 (round (* rem 0.34)))))
              (setq rem (- rem d))
              (condition-case nil
                  (if advance (pixel-scroll-precision-scroll-down d)
                    (pixel-scroll-precision-scroll-up d))
                ((beginning-of-buffer end-of-buffer) (throw 'web--settled nil)))
              (redisplay t)
              (sleep-for 0.009))))
        (web--snap-vscroll win)))))

(defun web--scroll-page (dir)
  "Smoothly scroll DIR pages (1 = down/C-v, -1 = up/M-v).
A lerp-to-target glide: each step covers `web-scroll-page-lerp' of the
remaining distance (capped to ~1 line), so the motion starts at full
speed and *decelerates* into a soft stop — momentum, not a flat linear
glide.  Then `web--settle' eases the leftover vscroll to a line boundary
(rest = vscroll 0, so a later cursor move neither shifts the buffer nor
leaves stale rows), and `force-window-update' makes the client repaint
the settled frame in full.

Per-step deltas are kept small (≈1 line): one redisplay that scrolls
several lines at once trips Emacs's row-reuse path and corrupts the
client render (overlapping/missing rows) — this is why the delta is
capped, not just eased.  The frame output coalesces, so the many small
redisplays still deliver only ~a dozen frames over the wire."
  (let* ((win (selected-window))
         (lh (max 1 (frame-char-height)))
         (lines (max 1 (- (floor (window-text-height win t) lh)
                          next-screen-context-lines)))
         (px (* dir lines lh)))             ; signed page distance
    ;; Use Emacs's own smooth interpolation (clean + stable; the
    ;; hand-rolled lerp/settle variants corrupted the client render).
    (condition-case nil
        (pixel-scroll-precision-interpolate (- px) win 1)
      ((beginning-of-buffer end-of-buffer) nil))
    (web--snap-vscroll win)
    (force-window-update win)))

(defun web--split-scroll (orig delta &rest args)
  "Around-advice for the precision scroll primitives: scroll DELTA in
≤1-line chunks with a redisplay between each.  A single redisplay that
scrolls several lines at once trips Emacs's row-reuse path and corrupts
the web client's render (overlapping/missing rows) — which a fast
trackpad flick or wheel `click' (large per-event delta) would otherwise
do.  Small deltas (high-res trackpad, C-v's own loop) pass straight
through."
  (let ((maxd (max 6 (- (frame-char-height) 2))))
    (if (or (not (integerp delta)) (<= delta maxd))
        (apply orig delta args)
      (let ((remaining delta))
        (while (> remaining 0)
          (let ((d (min maxd remaining)))
            (apply orig d args)
            (setq remaining (- remaining d))
            (when (> remaining 0) (redisplay t))))))))

(defvar web--vscroll-snap-timer nil
  "Idle timer that snaps a leftover trackpad/wheel vscroll after a pause.")

(defun web--vscroll-snap-on-idle (&rest _)
  "Schedule a snap of the selected window's vscroll once scrolling pauses.
A trackpad/wheel precision scroll leaves a fractional vscroll; if left,
the next command resets it (shifting the buffer) with a partial redisplay
that leaves the web client stale.  When scrolling settles (Emacs goes
idle), snap to the nearest line boundary and force a full repaint, the
same way C-v/M-v do."
  (when web--vscroll-snap-timer (cancel-timer web--vscroll-snap-timer))
  (let ((win (selected-window)))
    (setq web--vscroll-snap-timer
          (run-with-idle-timer
           0.12 nil
           (lambda ()
             (setq web--vscroll-snap-timer nil)
             (when (and (window-live-p win)
                        (not (window-minibuffer-p win)))
               ;; Force a full RESEND while still vscrolled: with a nonzero
               ;; vscroll Emacs can't reuse rows (no_scrolling_p +
               ;; try_window_id bails), so every visible row is redrawn and
               ;; resent — restoring any row the client dropped mid-scroll
               ;; (e.g. the cursor's row when a scroll pushes it to the top
               ;; and its translated pixel_y transiently overlapped a
               ;; neighbour).  `force-window-update' alone can't do this:
               ;; Emacs's matrix still has the row, so it never redraws it.
               (when (> (window-vscroll win t) 0)
                 (redisplay t)
                 ;; …then snap the leftover vscroll to a line boundary.
                 (web--snap-vscroll win))
               (force-window-update win)))))))

(defun web-scroll-up-page ()
  "Smoothly scroll down by a whole-line page (like \\[scroll-up-command])."
  (interactive)
  (web--scroll-page 1))

(defun web-scroll-down-page ()
  "Smoothly scroll up by a whole-line page (like \\[scroll-down-command])."
  (interactive)
  (web--scroll-page -1))

;; Start the 60Hz redisplay timer after init completes.
;; This must not run during init or byte-compilation of user packages
;; will crash due to SIGALRM interference.
(add-hook 'after-init-hook
          (lambda ()
            (when (fboundp 'web--start-redisplay-timer)
              (web--start-redisplay-timer))
            ;; Disable cursor blinking to avoid expensive repeated
            ;; redisplay.  Each blink tick triggers redisplay_tab_bar
            ;; which runs full BiDi resolution on the tab bar string.
            (blink-cursor-mode -1)
            ;; Smooth, subpixel scrolling.  The web backend transmits each
            ;; row's true pixel_y (= row->y) and the client clips windows,
            ;; so Emacs's own vscroll-based precision scrolling renders the
            ;; partial top/bottom rows correctly.  The wheel event carries a
            ;; (t . PIXELS) cons (webterm.c), which is what
            ;; `pixel-scroll-precision' reads to drive vscroll.
            (require 'pixel-scroll)
            (setq pixel-scroll-precision-use-momentum t
                  pixel-scroll-precision-interpolate-page t
                  pixel-scroll-precision-large-scroll-height 40.0
                  pixel-scroll-precision-interpolation-total-time 0.22
                  ;; CRITICAL for the web backend: the interpolation loop
                  ;; calls `(redisplay)' every `between-scroll' seconds,
                  ;; and each web redisplay serializes the whole window to
                  ;; JSON and ships it over the WebSocket.  The 0.001s
                  ;; (1ms = ~1000fps) default floods the pipeline and makes
                  ;; C-v/M-v lag badly.  ~100fps is smooth and the canvas
                  ;; render is only ~0.8ms, so the pipeline keeps up.
                  pixel-scroll-precision-interpolation-between-scroll 0.010)
            (pixel-scroll-precision-mode 1)
            ;; Smooth keyboard page scrolling.  This Emacs is web-only, so a
            ;; global remap is safe.
            (global-set-key [remap scroll-up-command]
                            #'web-scroll-up-page)
            (global-set-key [remap scroll-down-command]
                            #'web-scroll-down-page)
            ;; A bit of inertia for wheel/trackpad scrolling.
            (setq pixel-scroll-precision-momentum-seconds 1.2
                  pixel-scroll-precision-momentum-min-velocity 10.0)
            ;; After a trackpad/wheel scroll settles, snap the leftover
            ;; fractional vscroll to a line boundary + full repaint, so a
            ;; later cursor move neither shifts the buffer nor leaves the
            ;; client showing stale rows (same fix C-v/M-v do inline).
            ;; Advise the low-level scroll primitives — wheel events,
            ;; momentum, and C-v all funnel through them — so the snap is
            ;; (re)scheduled by *every* pixel scroll and only fires once all
            ;; scrolling, including inertia, has stopped.
            (dolist (fn '(pixel-scroll-precision-scroll-down
                          pixel-scroll-precision-scroll-up))
              (advice-add fn :after #'web--vscroll-snap-on-idle))
            ;; Flashy motion effects: jump beacons, isearch flashes, and
            ;; smooth scroll-to-definition (the cursor comet trail and
            ;; caret glide are client-only).  Require here, at runtime:
            ;; web-win.el's top level runs at DUMP time (noninteractive),
            ;; so a top-level `require' would be skipped.
            (ignore-errors
              (require 'web-fx)
              (web-fx-setup))
            ;; Ensure *scratch* has initial-scratch-message.
            ;; This is normally done by command-line-1 in startup.el, but
            ;; that code may not run when the web backend auto-detects
            ;; (no -nw flag to trigger tty, no X display to trigger X).
            (when (and initial-scratch-message
                       (get-buffer "*scratch*"))
              (with-current-buffer "*scratch*"
                (when (zerop (buffer-size))
                  (insert (substitute-command-keys initial-scratch-message))
                  (set-buffer-modified-p nil))))))

;;;; Icon font loading for all-the-icons and similar packages.

(defvar web--icon-font-dirs
  '("~/.local/share/fonts/"
    "~/.fonts/"
    "/usr/share/fonts/truetype/")
  "Directories to search for icon font files.")

(defvar web--icon-font-names
  '(("all-the-icons" . "all-the-icons.ttf")
    ("file-icons" . "file-icons.ttf")
    ("FontAwesome" . "FontAwesome.ttf")
    ("github-octicons" . "octicons.ttf")
    ("Material Icons" . "material-design-icons.ttf")
    ("Weather Icons" . "weathericons-regular-webfont.ttf"))
  "Alist of (FAMILY-NAME . FILENAME) for icon fonts to load.")

(defun web--load-icon-fonts ()
  "Load icon font files into the web display browser."
  (when (fboundp 'web-load-font)
    (dolist (entry web--icon-font-names)
      (let ((name (car entry))
            (file (cdr entry)))
        ;; Search in all-the-icons package directories first.
        (let ((found nil))
          (dolist (dir (append
                        ;; Common package manager font directories.
                        (let ((dirs nil))
                          (dolist (d '("~/.emacs.d/straight/repos/all-the-icons.el/fonts/"
                                       "~/.emacs.d/elpa/all-the-icons-*/fonts/"
                                       "~/.config/emacs/straight/repos/all-the-icons.el/fonts/"))
                            (let ((expanded (file-expand-wildcards d)))
                              (when expanded
                                (setq dirs (append dirs expanded)))))
                          dirs)
                        web--icon-font-dirs))
            (unless found
              (let ((path (expand-file-name file dir)))
                (when (file-exists-p path)
                  (condition-case nil
                      (progn
                        (web-load-font name path)
                        (setq found t))
                    (error nil)))))))))))

(add-hook 'after-init-hook #'web--load-icon-fonts)

;;;; JavaScript widgets.

(defun web-insert-javascript-widget (code &optional fallback)
  "Insert FALLBACK or CODE, and evaluate CODE in web displays.
Vanilla Emacs sees ordinary buffer text.  The web backend also receives
CODE through `web-eval-javascript'."
  (insert (or fallback code))
  (when (and (fboundp 'web-eval-javascript)
             (display-graphic-p))
    (web-eval-javascript code)))

;;; Selection / system clipboard.
;;
;; The web frame is a window-system frame, so `interprogram-cut-function'
;; is `gui-select-text', which dispatches to these gui-backend methods.
;; Without them M-w / C-w reached only the kill ring -- the browser (and
;; thus the OS clipboard) never saw the text.  Only CLIPBOARD is mirrored
;; to the browser via `web-set-clipboard'; PRIMARY stays inside Emacs, so
;; `select-active-regions' doesn't clobber the system clipboard on every
;; selection change (the browser/macOS has no PRIMARY anyway).  The
;; browser pushes the OS clipboard back on paste, landing in
;; `web-clipboard-text', which feeds get-selection so yank picks up
;; external copies.

(defvar web--selections nil
  "Alist of (SELECTION . STRING) the web backend currently owns.")

(cl-defmethod gui-backend-set-selection (selection value
                                         &context (window-system web))
  (if (null value)
      (setq web--selections (assq-delete-all selection web--selections))
    (setf (alist-get selection web--selections) value)
    (when (and (eq selection 'CLIPBOARD) (fboundp 'web-set-clipboard))
      (web-set-clipboard value)
      ;; Reflect our own copy into web-clipboard-text immediately, so a
      ;; following yank sees it without waiting for a focus round-trip.
      (when (boundp 'web-clipboard-text)
        (setq web-clipboard-text value)))))

(cl-defmethod gui-backend-get-selection (selection-symbol target-type
                                         &context (window-system web))
  (cond
   ((eq target-type 'TARGETS) (vector 'TARGETS 'STRING))
   ((eq target-type 'TIMESTAMP) 0)
   ;; CLIPBOARD: web-clipboard-text is the OS clipboard, kept current by
   ;; the client syncing it on window focus (so copies made in other
   ;; apps win over a stale local selection).  Fall back to our own last
   ;; copy only if nothing has been synced yet.
   ((eq selection-symbol 'CLIPBOARD)
    (or (and (stringp (bound-and-true-p web-clipboard-text))
             (> (length web-clipboard-text) 0)
             web-clipboard-text)
        (alist-get 'CLIPBOARD web--selections)))
   (t (alist-get selection-symbol web--selections))))

(cl-defmethod gui-backend-selection-owner-p (selection
                                             &context (window-system web))
  (and (assq selection web--selections) t))

(cl-defmethod gui-backend-selection-exists-p (selection
                                              &context (window-system web))
  (or (and (assq selection web--selections) t)
      (and (eq selection 'CLIPBOARD)
           (stringp (bound-and-true-p web-clipboard-text))
           (> (length web-clipboard-text) 0))))

;; Load web-widgets (skip during dump when noninteractive).
;; web-fx is required at runtime from the after-init-hook above, not
;; here: this top level also runs at dump time, where the require would
;; be skipped and never retried.
(unless noninteractive
  (require 'web-widgets))

(provide 'web-win)

;;; web-win.el ends here
