;;; web-term.el --- Terminal emulator backed by libvterm -*- lexical-binding: t; -*-

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

;; A terminal emulator for the Emacs web display backend that uses
;; libvterm (the C library) for VT parsing and renders output as a
;; real Emacs buffer with faces.  True color, alternate screen, and
;; proper escape sequence handling are all supported via libvterm.
;;
;; Usage:
;;   M-x web-term         — open a terminal with $SHELL
;;   M-x web-term-tmux    — open a terminal attached to a tmux session
;;
;; In the terminal:
;;   C-c C-t — toggle copy mode (navigate with normal Emacs keys)
;;   C-c C-c — send interrupt (SIGINT)
;;   C-x, C-c prefix keys pass through to Emacs

;;; Code:

(require 'cl-lib)

;;; — Buffer-local variables —

(defvar-local web-term--handle nil
  "Integer handle to the libvterm C instance.")

(defvar-local web-term--process nil
  "The PTY process for this terminal.")

(defvar-local web-term--copy-mode nil
  "Non-nil when copy mode is active.")

(defvar-local web-term--rows 24
  "Current terminal rows.")

(defvar-local web-term--cols 80
  "Current terminal columns.")

;;; — Keymap —

(defvar web-term-mode-map
  (let ((map (make-sparse-keymap)))
    ;; Self-inserting characters.
    (define-key map [remap self-insert-command] #'web-term-self-insert)
    ;; Special keys.
    (define-key map (kbd "RET")        #'web-term-send-return)
    (define-key map (kbd "TAB")        #'web-term-send-tab)
    (define-key map (kbd "DEL")        #'web-term-send-backspace)
    (define-key map (kbd "<backspace>") #'web-term-send-backspace)
    (define-key map (kbd "<up>")       #'web-term-send-up)
    (define-key map (kbd "<down>")     #'web-term-send-down)
    (define-key map (kbd "<left>")     #'web-term-send-left)
    (define-key map (kbd "<right>")    #'web-term-send-right)
    (define-key map (kbd "<home>")     #'web-term-send-home)
    (define-key map (kbd "<end>")      #'web-term-send-end)
    (define-key map (kbd "<prior>")    #'web-term-send-prior)
    (define-key map (kbd "<next>")     #'web-term-send-next)
    (define-key map (kbd "<escape>")   #'web-term-send-escape)
    (define-key map (kbd "<delete>")   #'web-term-send-delete)
    (define-key map (kbd "<insert>")   #'web-term-send-insert)
    ;; Function keys.
    (dolist (n (number-sequence 1 12))
      (define-key map (kbd (format "<f%d>" n))
                  (let ((fn n))
                    (lambda ()
                      (interactive)
                      (web-term--send-key (intern (format "f%d" fn)) 0)))))
    ;; C-a through C-z (except C-c, C-x which stay with Emacs,
    ;; and C-i, C-m which are TAB and RET — handled above).
    (dolist (c (number-sequence ?a ?z))
      (unless (memq c '(?c ?i ?m ?x))
        (define-key map (kbd (format "C-%c" c))
                    (let ((ch c))
                      (lambda ()
                        (interactive)
                        (web-term--send-key ch 4)))))) ; 4 = ctrl
    ;; C-c C-c sends interrupt.
    (define-key map (kbd "C-c C-c") #'web-term-send-C-c)
    ;; Copy mode toggle.
    (define-key map (kbd "C-c C-t") #'web-term-copy-mode)
    map)
  "Keymap for `web-term-mode'.")

;;; — Major mode —

(define-derived-mode web-term-mode fundamental-mode "WebTerm"
  "Terminal emulator backed by libvterm.

\\<web-term-mode-map>
All keyboard input is sent to the terminal except for:
  \\[web-term-copy-mode] — toggle copy mode
  \\[web-term-send-C-c]  — send C-c to terminal
  C-x prefix           — Emacs commands
  C-c prefix           — Emacs commands (except C-c C-c, C-c C-t)"
  :interactive nil
  (setq-local buffer-read-only t)
  (setq-local scroll-margin 0)
  (setq-local scroll-conservatively 101)
  (setq-local truncate-lines t)
  (setq-local cursor-type 'box)
  (setq-local show-trailing-whitespace nil)
  (setq-local indicate-empty-lines nil)
  ;; Override line-spacing so Emacs and browser agree on line height.
  ;; The browser uses CSS lineHeight for internal spacing; extra Emacs
  ;; line-spacing causes a row-count mismatch (fewer lines visible than
  ;; the window body-height reports).
  (setq-local line-spacing 0)
  (buffer-disable-undo)
  ;; Give our keymap highest priority so minor modes like windmove-mode
  ;; can't steal arrow keys and other terminal-bound keys.
  (setq-local emulation-mode-map-alists
              (list (list (cons 'web-term-mode web-term-mode-map)))))

;;; — Key sending helpers —

(defun web-term--send-key (key mods)
  "Send KEY with MODS to the terminal.
KEY is a character (integer) or a symbol.
MODS is a bitmask: 1=shift, 2=alt, 4=ctrl."
  (when web-term--handle
    (let ((out (web-vterm-key-input web-term--handle key mods)))
      (when (and out (> (length out) 0) web-term--process
                 (process-live-p web-term--process))
        (process-send-string web-term--process out)))))

(defun web-term-self-insert ()
  "Send the last typed character to the terminal."
  (interactive)
  (let ((ch last-command-event))
    (when (characterp ch)
      (web-term--send-key ch 0))))

(defun web-term-send-return ()    (interactive) (web-term--send-key 'return 0))
(defun web-term-send-tab ()       (interactive) (web-term--send-key 'tab 0))
(defun web-term-send-backspace () (interactive) (web-term--send-key 'backspace 0))
(defun web-term-send-escape ()    (interactive) (web-term--send-key 'escape 0))
(defun web-term-send-up ()        (interactive) (web-term--send-key 'up 0))
(defun web-term-send-down ()      (interactive) (web-term--send-key 'down 0))
(defun web-term-send-left ()      (interactive) (web-term--send-key 'left 0))
(defun web-term-send-right ()     (interactive) (web-term--send-key 'right 0))
(defun web-term-send-home ()      (interactive) (web-term--send-key 'home 0))
(defun web-term-send-end ()       (interactive) (web-term--send-key 'end 0))
(defun web-term-send-prior ()     (interactive) (web-term--send-key 'prior 0))
(defun web-term-send-next ()      (interactive) (web-term--send-key 'next 0))
(defun web-term-send-delete ()    (interactive) (web-term--send-key 'delete 0))
(defun web-term-send-insert ()    (interactive) (web-term--send-key 'insert 0))

(defun web-term-send-C-c ()
  "Send C-c (interrupt) to the terminal."
  (interactive)
  (web-term--send-key ?c 4))

;;; — Cursor positioning —

(defun web-term--cursor-to-point (row col)
  "Convert terminal ROW and COL (0-indexed) to a buffer position."
  (save-restriction
    (widen)
    (goto-char (point-min))
    (forward-line row)
    (let ((line-end (line-end-position)))
      (min (+ (point) col) line-end))))

;;; — Process filter and sentinel —

(defun web-term--make-filter (handle buf)
  "Return a process filter that feeds data to libvterm.
HANDLE is the vterm instance, BUF is the terminal buffer."
  (lambda (_proc output)
    (when (buffer-live-p buf)
      (with-current-buffer buf
        ;; Feed raw bytes to libvterm parser.
        (web-vterm-write handle output)
        ;; Sync screen to buffer.
        (let ((inhibit-read-only t)
              (inhibit-modification-hooks t))
          (web-vterm-update handle buf))
        ;; Move point to cursor position.
        (let ((pos (web-vterm-get-cursor handle)))
          (let ((new-pos (web-term--cursor-to-point (car pos) (cdr pos))))
            (goto-char new-pos)
            ;; Pin window: set BOTH window-point and window-start so Emacs
            ;; doesn't auto-scroll.  goto-char only updates buffer-point,
            ;; not the window-point of non-selected windows.
            (let ((w (get-buffer-window buf)))
              (when w
                (set-window-point w new-pos)
                (set-window-start w (point-min) t)))))
        ;; Update buffer name with terminal title.
        (let ((title (web-vterm-get-title handle)))
          (when title
            (rename-buffer (format "*web-term: %s*" title) t)))
        ;; Send any pending output (keyboard echo, etc.).
        (let ((out (web-vterm-get-output handle)))
          (when (and out (> (length out) 0)
                     web-term--process
                     (process-live-p web-term--process))
            (process-send-string web-term--process out)))))))

(defun web-term--make-sentinel (buf)
  "Return a process sentinel for terminal buffer BUF."
  (lambda (_proc event)
    (when (buffer-live-p buf)
      (with-current-buffer buf
        (let ((inhibit-read-only t))
          (goto-char (point-max))
          (insert (format "\n[Process %s]\n" (string-trim event))))
        (when web-term--handle
          (web-vterm-destroy web-term--handle)
          (setq web-term--handle nil))))))

;;; — Window resize —

(defun web-term--fit-to-window ()
  "Resize terminal to fit current window."
  (when (and web-term--handle web-term--process
             (process-live-p web-term--process))
    (let ((w (1- (window-body-width)))  ;; reserve 1 col for truncation indicator
          (h (window-body-height)))
      (when (and (> w 0) (> h 0)
                 (or (/= w web-term--cols) (/= h web-term--rows)))
        (setq web-term--rows h
              web-term--cols w)
        (web-vterm-resize web-term--handle h w)
        (set-process-window-size web-term--process h w)
        ;; Re-init buffer with new dimensions and sync.
        (let ((inhibit-read-only t)
              (inhibit-modification-hooks t))
          (web-vterm-init-buffer web-term--handle (current-buffer))
          (web-vterm-update web-term--handle (current-buffer)))))))

(defun web-term--window-size-change (_frame)
  "Hook for `window-size-change-functions'."
  (dolist (win (window-list))
    (with-current-buffer (window-buffer win)
      (when (eq major-mode 'web-term-mode)
        (web-term--fit-to-window)))))

;;; — Copy mode —

(defun web-term-copy-mode ()
  "Toggle copy mode for navigating terminal output.
In copy mode, normal Emacs navigation keys work and you can
use `isearch', `mark', etc.  Press \\[web-term-copy-mode] or `q' to exit."
  (interactive)
  (if web-term--copy-mode
      (progn
        (setq web-term--copy-mode nil)
        (use-local-map web-term-mode-map)
        (setq cursor-type 'box)
        (message "Copy mode OFF"))
    (setq web-term--copy-mode t)
    (use-local-map (let ((map (make-sparse-keymap)))
                     (set-keymap-parent map special-mode-map)
                     (define-key map (kbd "C-c C-t") #'web-term-copy-mode)
                     (define-key map "q" #'web-term-copy-mode)
                     map))
    (setq cursor-type 'bar)
    (message "Copy mode ON — navigate freely, q to exit")))

;;; — Public entry points —

;;;###autoload
(defun web-term (&optional cmd)
  "Open a libvterm-backed terminal.
Optional CMD specifies the shell command to run (default: $SHELL)."
  (interactive)
  (unless (fboundp 'web-vterm-new)
    (error "web-vterm not available (Emacs not built with --with-web and libvterm)"))
  (let* ((shell (or cmd (getenv "SHELL") "/bin/zsh"))
         (buf (generate-new-buffer "*web-term*"))
         (handle (web-vterm-new 24 80)))
    (with-current-buffer buf
      (web-term-mode)
      (setq web-term--handle handle)
      ;; Initialize buffer with empty screen.
      (let ((inhibit-read-only t)
            (inhibit-modification-hooks t))
        (web-vterm-init-buffer handle buf))
      ;; Start shell process with TERM set for libvterm.  Emacs
      ;; intentionally clears ONLCR on PTYs (child_setup_tty), but
      ;; terminal emulators need it so LF produces CR+LF; restore it
      ;; before exec'ing the command rather than typing an stty
      ;; command at whatever is already running (fatal for tmux
      ;; attach: the keystrokes would land inside the session).
      (let ((process-environment (cons "TERM=xterm-256color"
                                       process-environment)))
        (setq web-term--process
              (make-process
               :name "web-term"
               :buffer nil  ; don't auto-insert output
               :command (list "sh" "-c"
                              (concat "stty onlcr 2>/dev/null; "
                                      (if (string-match-p " " shell)
                                          shell
                                        (concat "exec " shell))))
               :connection-type 'pty
               :coding 'no-conversion
               :filter (web-term--make-filter handle buf)
               :sentinel (web-term--make-sentinel buf))))
      ;; Set initial PTY size.
      (set-process-window-size web-term--process 24 80)
      (process-put web-term--process 'adjust-window-size-function #'ignore))
    (switch-to-buffer buf)
    ;; Size to current window.
    (web-term--fit-to-window)
    ;; Register resize hook.
    (add-hook 'window-size-change-functions #'web-term--window-size-change)
    buf))

;;;###autoload
(defun web-term-tmux (&optional session-name)
  "Open a terminal backed by a tmux session.
SESSION-NAME defaults to a random name."
  (interactive "sSession name (empty for random): ")
  (let ((name (if (or (null session-name) (string-empty-p session-name))
                  (format "emacs-%d" (random 10000))
                session-name)))
    (web-term (format "tmux new-session -A -s %s"
                      (shell-quote-argument name)))))

;;; — Clipboard paste handling —

(defvar web-term--last-clipboard nil
  "Last value of `web-clipboard-text' we processed.")

(defun web-term--process-clipboard ()
  "Check for new clipboard paste and send it to the active terminal.
Called by a timer to bridge browser paste/drop events to the PTY."
  (when (and (boundp 'web-clipboard-text)
             web-clipboard-text
             (stringp web-clipboard-text)
             (> (length web-clipboard-text) 0))
    ;; Grab and clear immediately so repeated pastes always work.
    (let ((text web-clipboard-text))
      (setq web-clipboard-text nil)
      ;; Find the active web-term buffer and send paste to its PTY.
      (let ((buf (or (and (eq major-mode 'web-term-mode) (current-buffer))
                     (with-current-buffer (window-buffer (selected-window))
                       (when (eq major-mode 'web-term-mode)
                         (current-buffer))))))
        (when (and buf (buffer-live-p buf))
          (with-current-buffer buf
            (when (and web-term--process (process-live-p web-term--process))
              ;; Send with bracketed paste markers so apps like Claude Code
              ;; can distinguish paste from typing.
              (process-send-string web-term--process "\e[200~")
              (process-send-string web-term--process text)
              (process-send-string web-term--process "\e[201~"))))))))

(defvar web-term--clipboard-timer nil
  "Timer for polling clipboard paste events.")

(defun web-term--start-clipboard-timer ()
  "Start the clipboard paste polling timer."
  (unless web-term--clipboard-timer
    (setq web-term--clipboard-timer
          (run-with-timer 0.1 0.1 #'web-term--process-clipboard))))

;; Start the timer when this file loads.
(web-term--start-clipboard-timer)

;;; — Cleanup —

(defun web-term--kill-buffer-hook ()
  "Clean up when a web-term buffer is killed."
  (when (and web-term--handle (eq major-mode 'web-term-mode))
    (when (and web-term--process (process-live-p web-term--process))
      (delete-process web-term--process))
    (condition-case nil
        (web-vterm-destroy web-term--handle)
      (error nil))
    (setq web-term--handle nil)))

(add-hook 'kill-buffer-hook #'web-term--kill-buffer-hook)

(provide 'web-term)

;;; web-term.el ends here
