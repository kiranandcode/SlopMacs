;;; slop-term.el --- tmux-backed terminal for web display -*- lexical-binding: t -*-

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

;; Terminal mode backed by tmux sessions using libvterm.  The tmux
;; session survives Emacs restarts, so running processes (e.g. claude
;; CLI) persist across hot-reloads.

;;; Code:

(require 'web-term)

(defvar-local slop-term-session nil
  "Name of the tmux session backing this terminal buffer.")

;;;###autoload
(defun slop-term (&optional name)
  "Open a terminal backed by a tmux session NAME.
Uses libvterm for full escape sequence support.
The tmux session survives Emacs restarts."
  (interactive "sSession name: ")
  (let* ((session-name (if (and name (not (string-empty-p name)))
                           name
                         (format "emacs-%d" (random 10000)))))
    ;; web-term-tmux creates/attaches the tmux session via libvterm.
    (let ((buf (web-term-tmux session-name)))
      (with-current-buffer buf
        (setq-local slop-term-session session-name)
        (setq-local desktop-save-buffer #'slop-term-desktop-save))
      buf)))

;;; Desktop persistence.

(defun slop-term-desktop-save (_dirname)
  "Save slop-term state for desktop restore."
  (list (cons 'session slop-term-session)))

(defun slop-term-desktop-restore (_file-name _buffer-name misc)
  "Restore a slop-term buffer by reattaching to its tmux session."
  (let ((session (cdr (assq 'session misc))))
    (when session
      (slop-term session))))

(add-to-list 'desktop-buffer-mode-handlers
             '(web-term-mode . slop-term-desktop-restore))

(provide 'slop-term)

;;; slop-term.el ends here
