;;; session-reload.el --- restart Emacs in place, preserving the session -*- lexical-binding: t -*-

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

;; Hot-reload workflow for hacking on Emacs itself:
;;
;;   1. Rebuild src/emacs (e.g. Claude Code edits C, runs make).
;;   2. M-x session-reload  (or: emacsclient -e '(session-reload)',
;;      or: ./slop-reload.sh which also rebuilds).
;;   3. Emacs saves the session -- visited files, buffer list, point,
;;      window layout, non-file buffer contents -- and exits with
;;      code 42.
;;   4. web-display/emacs-wrapper.sh sees code 42 and starts the NEW
;;      binary at the same path, which restores the session.
;;
;; The web/Electron client reconnects to the new instance's proxy, so
;; from the user's point of view the editor blinks and comes back with
;; the same buffers and windows, running the new C code.
;;
;; Why not pdumper?  A pdump file is fingerprint-locked to the exact
;; binary that produced it, so the old instance's dump can never be
;; loaded by the freshly rebuilt binary; and pdumper cannot capture
;; window-system state, file descriptors, or subprocesses.  Session
;; state must cross the rebuild boundary at the Lisp level.
;; Subprocesses you care about should live in tmux (see slop-term.el),
;; which survives the restart and is reattached on restore.

;;; Code:

(require 'desktop)

(defgroup session-reload nil
  "Restart Emacs in place, preserving the session."
  :group 'convenience)

(defcustom session-reload-directory
  (expand-file-name "web-session" user-emacs-directory)
  "Directory holding saved session state across reloads."
  :type 'directory)

(defcustom session-reload-save-nonfile-buffers t
  "Non-nil means save and restore the contents of non-file buffers.
Buffers whose names start with a space or `*' (except *scratch*) are
skipped."
  :type 'boolean)

(defvar session-reload--flag-file nil)
(defvar session-reload--window-file nil)
(defvar session-reload--buffers-file nil)

(defun session-reload--paths ()
  (setq session-reload--flag-file
        (expand-file-name "pending-reload" session-reload-directory)
        session-reload--window-file
        (expand-file-name "window-state.eld" session-reload-directory)
        session-reload--buffers-file
        (expand-file-name "nonfile-buffers.eld" session-reload-directory)))

(defun session-reload--interesting-nonfile-buffers ()
  (cl-remove-if-not
   (lambda (buf)
     (let ((name (buffer-name buf)))
       (and (not (buffer-file-name buf))
            (not (string-prefix-p " " name))
            (or (string= name "*scratch*")
                (not (string-prefix-p "*" name)))
            (buffer-local-value 'buffer-undo-list buf) ; has been touched
            (> (buffer-size buf) 0))))
   (buffer-list)))

(defun session-reload-save ()
  "Save the current session to `session-reload-directory'."
  (session-reload--paths)
  (make-directory session-reload-directory t)
  ;; Save all modified file-visiting buffers, no questions asked.
  (save-some-buffers t)
  ;; Window layout of the selected frame.
  (with-temp-file session-reload--window-file
    (let ((print-circle t) (print-length nil) (print-level nil))
      (prin1 (window-state-get (frame-root-window (selected-frame)) t)
             (current-buffer))))
  ;; Contents of non-file buffers (*scratch* and friends).
  (when session-reload-save-nonfile-buffers
    (with-temp-file session-reload--buffers-file
      (let ((print-length nil) (print-level nil) (print-circle t)
            (out '()))
        (dolist (buf (session-reload--interesting-nonfile-buffers))
          (with-current-buffer buf
            (push (list (buffer-name) (buffer-string) (point) major-mode)
                  out)))
        (prin1 out (current-buffer)))))
  ;; Visited files, point positions, buffer order.  A stale lock from
  ;; a force-killed predecessor must not block or prompt.
  (let ((lock (expand-file-name ".emacs.desktop.lock"
                                session-reload-directory)))
    (when (file-exists-p lock)
      (ignore-errors (delete-file lock))))
  (let ((desktop-restore-frames nil)    ; window layout handled above
        (desktop-save-mode nil))
    (desktop-save session-reload-directory t))
  (write-region "" nil session-reload--flag-file nil 'silent))

;;;###autoload
(defun session-reload ()
  "Save the session and restart Emacs via the wrapper.
The wrapper (web-display/emacs-wrapper.sh) re-executes the Emacs
binary, picking up a rebuilt one, and the new instance restores the
session."
  (interactive)
  (session-reload-save)
  (kill-emacs 42))

(defun session-reload--restore ()
  (session-reload--paths)
  ;; Visited files, point, buffer order.
  (let ((desktop-restore-frames nil)
        (desktop-load-locked-desktop t)
        (desktop-dirname session-reload-directory))
    (ignore-errors (desktop-read session-reload-directory)))
  ;; Non-file buffer contents.
  (when (file-readable-p session-reload--buffers-file)
    (dolist (spec (with-temp-buffer
                    (insert-file-contents session-reload--buffers-file)
                    (ignore-errors (read (current-buffer)))))
      (pcase-let ((`(,name ,content ,pt ,mode) spec))
        (with-current-buffer (get-buffer-create name)
          ;; Replace pristine content (e.g. *scratch*'s initial
          ;; message); leave buffers something else already filled.
          (when (or (= (buffer-size) 0)
                    (and (string= name "*scratch*")
                         (not (buffer-modified-p))))
            (erase-buffer)
            (insert content)
            (set-buffer-modified-p nil)
            (goto-char (min pt (point-max)))
            (when (and mode (fboundp mode))
              (ignore-errors (funcall mode))))))))
  ;; Window layout.
  (when (file-readable-p session-reload--window-file)
    (ignore-errors
      (window-state-put
       (with-temp-buffer
         (insert-file-contents session-reload--window-file)
         (read (current-buffer)))
       (frame-root-window (selected-frame)) 'safe))))

(defcustom session-reload-server-name "web-emacs"
  "Server name for the hot-reloadable web Emacs instance.
Deliberately distinct from the default so that emacsclient commands
aimed at this instance (slop-reload.sh uses \"emacsclient -s
web-emacs\") can never reach another Emacs session running on the
same machine."
  :type 'string)

;;;###autoload
(defun session-reload-init ()
  "Set up hot-reload support; called by emacs-wrapper.sh at startup.
Starts the Emacs server under `session-reload-server-name' (so
slop-reload.sh can reach us via emacsclient without ever touching
another running Emacs) and restores a pending saved session, if
any."
  (session-reload--paths)
  (require 'server)
  ;; If the user's init started a server under the default name, move
  ;; it: this instance must never own the socket the user's other
  ;; Emacs sessions (and their emacsclient muscle memory) rely on.
  (when (and (bound-and-true-p server-process)
             (not (equal server-name session-reload-server-name)))
    (ignore-errors (server-start t))    ; stop the misnamed server
    (setq server-process nil))
  (unless (bound-and-true-p server-process)
    (setq server-name session-reload-server-name)
    (if (server-running-p)
        ;; A live instance already owns the socket; don't hijack it.
        (message "session-reload: another %s server is running; \
emacsclient control disabled for this instance" server-name)
      ;; Remove a stale socket (e.g. left by a force-killed
      ;; predecessor), then start.
      (ignore-errors (server-force-delete))
      (ignore-errors (server-start))))
  (when (file-exists-p session-reload--flag-file)
    (delete-file session-reload--flag-file)
    (session-reload--restore)))

(provide 'session-reload)

;;; session-reload.el ends here
