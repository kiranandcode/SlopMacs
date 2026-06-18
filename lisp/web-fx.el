;;; web-fx.el --- Flashy motion effects for the web display -*- lexical-binding: t -*-

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

;; Cosmetic motion feedback for the browser-backed display: a jump
;; beacon when navigating (xref / imenu / next-error), a flash on
;; isearch matches, and smooth scroll-to-definition.  The beacons and
;; flashes are hints sent to the client over the generic
;; `web-tldraw-send' channel as {"type":"fx", ...} messages; the client
;; (fx.js / renderEngine.js) draws them on a transparent overlay.  The
;; cursor comet trail and caret glide live entirely on the client and
;; need no help from here.
;;
;; Coordinates are computed as ABSOLUTE FRAME PIXELS (window text-area
;; origin via `window-inside-pixel-edges' plus the position's pixel
;; offset via `posn-at-point'), which is exactly the coordinate space of
;; the client's text canvas — so the client draws them verbatim.

;;; Code:

(require 'pixel-scroll)

(defgroup web-fx nil
  "Flashy motion effects for the web display backend."
  :group 'web)

(defcustom web-fx-enabled t
  "When non-nil, emit jump beacons and search flashes to the web client."
  :type 'boolean :group 'web-fx)

(defcustom web-fx-smooth-scroll t
  "When non-nil, animate same-buffer navigation jumps as a smooth scroll."
  :type 'boolean :group 'web-fx)

(defcustom web-fx-smooth-scroll-max-screens 5.0
  "Skip the smooth-scroll animation for jumps farther than this many
windowfuls (they snap instantly instead — animating a very distant jump
would be slow and disorienting)."
  :type 'number :group 'web-fx)

;;;; Low-level: pixel geometry + emit.

(defun web-fx--point-pixel (pos win)
  "Absolute frame-pixel (X . Y) of POS in WIN, or nil if not visible."
  (when (window-live-p win)
    (let ((xy (ignore-errors (posn-x-y (posn-at-point pos win)))))
      (when xy
        (let ((edges (window-inside-pixel-edges win)))
          (cons (+ (nth 0 edges) (car xy))
                (+ (nth 1 edges) (cdr xy))))))))

(defun web-fx--face-color (face attr)
  "Return FACE's ATTR color as a string, or nil if unspecified."
  (let ((c (face-attribute face attr nil t)))
    (and (stringp c) c)))

(defun web-fx--emit (plist &optional color)
  "Send PLIST (a JSON object) to the web client, adding COLOR if given."
  (when (and web-fx-enabled
             (display-graphic-p)
             (fboundp 'web-tldraw-send))
    (when (stringp color)
      (setq plist (append plist (list :color color))))
    (ignore-errors (web-tldraw-send (json-serialize plist)))))

;;;; Beacon (jump destination pulse).

(defun web-fx-beacon (&optional pos win color)
  "Pulse a beacon at POS (default point) in WIN (default selected window).
Deferred to a near-immediate timer so the destination has been
redisplayed and `posn-at-point' can resolve it."
  (when web-fx-enabled
    (let ((win (or win (selected-window)))
          (pos (or pos (point))))
      (run-at-time 0.01 nil #'web-fx--beacon-now win pos color))))

(defun web-fx--beacon-now (win pos color)
  (let ((xy (web-fx--point-pixel pos win)))
    (when xy
      (web-fx--emit (list :type "fx" :effect "beacon"
                          :x (round (car xy)) :y (round (cdr xy))
                          :h (frame-char-height))
                    color))))

(defun web-fx--jump-beacon (&rest _)
  "Hook function: pulse a beacon at point after a navigation jump."
  (web-fx-beacon))

;;;; Isearch match flash.

(defvar web-fx--last-isearch nil
  "Last flashed isearch match start, to avoid re-flashing in place.")

(defun web-fx--isearch-flash ()
  "Flash the current isearch match on each successful search step."
  (when (and web-fx-enabled isearch-success isearch-other-end
             (number-or-marker-p isearch-other-end))
    (let* ((start (min (point) isearch-other-end))
           (end   (max (point) isearch-other-end)))
      (unless (eql start web-fx--last-isearch)
        (setq web-fx--last-isearch start)
        (let ((xy (web-fx--point-pixel start (selected-window))))
          (when xy
            (web-fx--emit
             (list :type "fx" :effect "flash"
                   :x (round (car xy)) :y (round (cdr xy))
                   :w (max (frame-char-width)
                           (* (- end start) (frame-char-width)))
                   :h (frame-char-height))
             (or (web-fx--face-color 'isearch :background)
                 (web-fx--face-color 'lazy-highlight :background)))))))))

(defun web-fx--isearch-reset ()
  (setq web-fx--last-isearch nil))

;;;; Smooth scroll-to-definition.

(defun web-fx--record-starts ()
  "Stash every live window's current start + buffer (for smooth scroll).
Runs on `pre-command-hook', so each window's parameter holds its start
as displayed *before* the about-to-run command.  Recording all windows
(not just the selected one) lets an after-jump hook that lands in
another window — e.g. selecting an occur/consult result — still find the
target window's pre-jump position."
  (when web-fx-smooth-scroll
    (dolist (win (window-list nil 'no-mini))
      (set-window-parameter win 'web-fx--prev-start (window-start win))
      (set-window-parameter win 'web-fx--prev-buf (window-buffer win)))))

(defun web-fx--screen-lines (from to)
  "Signed screen-line count between FROM and TO (TO below FROM is positive)."
  (if (<= from to)
      (count-screen-lines from to)
    (- (count-screen-lines to from))))

(defun web-fx--smooth-after-jump (&rest _)
  "If the just-finished jump scrolled within range, replay it smoothly.
Takes &rest args so it works both as a hook function and as `:after'
advice (e.g. on `recenter-top-bottom', which passes a prefix arg).
Uses the target window's pre-command start (recorded by
`web-fx--record-starts'), so it works even when the jump lands in a
window other than the one that was selected when the command began."
  (let* ((win (selected-window))
         (old-start (window-parameter win 'web-fx--prev-start))
         (old-buf (window-parameter win 'web-fx--prev-buf)))
    (when (and web-fx-smooth-scroll
               (bound-and-true-p pixel-scroll-precision-mode)
               (eq old-buf (current-buffer))
               (not (window-minibuffer-p win))
               (not executing-kbd-macro)
               (not (bound-and-true-p isearch-mode)))
      (let* ((new-start (window-start win))
             (lh (frame-char-height)))
        (when (and (integerp old-start) (integerp new-start)
                   (/= old-start new-start))
          (let* ((lines (web-fx--screen-lines old-start new-start))
                 (px (* lines lh))
                 (maxpx (* web-fx-smooth-scroll-max-screens
                           (window-text-height win t))))
            (when (and (> (abs px) (* 0.5 lh)) (< (abs px) maxpx))
              ;; Rewind to the pre-jump view (keep point at the
              ;; destination), then smoothly scroll to the new view.
              (set-window-start win old-start t)
              (condition-case nil
                  (pixel-scroll-precision-interpolate (- px) win 1)
                (error nil))
              ;; Guarantee the exact final position regardless of any
              ;; variable-height approximation in the line count.
              (set-window-start win new-start t))))))))

(defun web-fx--after-jump (&rest _)
  "Combined after-jump handler: smooth scroll, then beacon.
Takes &rest args so it works both as a hook function and as `:after'
advice on commands like `occur-mode-goto-occurrence'."
  (web-fx--smooth-after-jump)
  (web-fx--jump-beacon))

;;;; Wiring.

(defun web-fx-setup ()
  "Install the web-fx hooks.  Safe to call more than once."
  ;; Load the hosts first so their `defcustom' hook defaults (xref's
  ;; includes `recenter') are established before we append to them —
  ;; otherwise add-hook on the still-unbound variable would clobber the
  ;; default and break the jump.
  (require 'xref nil t)
  (require 'imenu nil t)
  ;; Continuously track each window's pre-command start, the source of
  ;; the "old" position for smooth scroll-to-target.
  (add-hook 'pre-command-hook #'web-fx--record-starts)
  ;; xref / imenu / next-error (occur, grep, flymake, …): smooth scroll
  ;; + beacon at the landing.
  (add-hook 'xref-after-jump-hook #'web-fx--after-jump t)
  (add-hook 'imenu-after-jump-hook #'web-fx--after-jump t)
  (add-hook 'next-error-hook #'web-fx--after-jump t)
  ;; consult (consult-line / consult-imenu / consult-ripgrep …): hook
  ;; only once the package is loaded, so its own `consult-after-jump-hook'
  ;; default (a recenter) is preserved.
  (with-eval-after-load 'consult
    (add-hook 'consult-after-jump-hook #'web-fx--after-jump t))
  ;; occur (M-s o): RET on a match goes through `occur-mode-goto-occurrence',
  ;; which has no after-jump hook — advise it (and its variants) directly.
  (with-eval-after-load 'replace
    (dolist (cmd '(occur-mode-goto-occurrence
                   occur-mode-goto-occurrence-other-window
                   occur-mode-display-occurrence))
      (when (fboundp cmd)
        (advice-add cmd :after #'web-fx--after-jump))))
  ;; C-l (recenter-top-bottom): animate the recenter as a smooth scroll
  ;; from the old window-start to the recentred one.  No beacon — point
  ;; didn't move, only the view.
  (when (fboundp 'recenter-top-bottom)
    (advice-add 'recenter-top-bottom :after #'web-fx--smooth-after-jump))
  ;; isearch: flash matches as you step through them.
  (add-hook 'isearch-update-post-hook #'web-fx--isearch-flash)
  (add-hook 'isearch-mode-end-hook #'web-fx--isearch-reset)
  (add-hook 'isearch-mode-hook #'web-fx--isearch-reset))

(provide 'web-fx)

;;; web-fx.el ends here
