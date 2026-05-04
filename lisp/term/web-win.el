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

;; Start the 60Hz redisplay timer after init completes.
;; This must not run during init or byte-compilation of user packages
;; will crash due to SIGALRM interference.
(add-hook 'after-init-hook
          (lambda ()
            (when (fboundp 'web--start-redisplay-timer)
              (web--start-redisplay-timer))
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

;; Load web-widgets (skip during dump when noninteractive).
(unless noninteractive
  (require 'web-widgets))

(provide 'web-win)

;;; web-win.el ends here
