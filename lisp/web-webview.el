;;; web-webview.el --- embed browser views in buffers (web backend) -*- lexical-binding: t -*-

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

;; On the web display backend the "screen" is a browser, so a buffer
;; can host real web content: any buffer whose buffer-local
;; `web-webview-url' is a string gets an iframe overlaid on its
;; window's body by the client (positioned from the window's pixel
;; geometry, which the backend already streams).  The mode line stays
;; visible underneath, so the buffer behaves like any other: it can be
;; split away, switched out, deleted -- the overlay follows.
;;
;;   M-x web-webview-open RET https://example.com RET
;;   M-x web-org-roam-graph   ; org-roam-ui graph in a buffer
;;
;; Input caveat: keys and clicks inside the iframe go to the embedded
;; page (that is what makes the graph pannable/zoomable); click on any
;; Emacs text (or the mode line) to give input back to Emacs.

;;; Code:

(defvar-local web-webview-url nil
  "URL the web client overlays as an iframe over this buffer's window.
Read by the C layer (webterm.c) during redisplay; nil means no
overlay.  Use `web-webview-open' rather than setting this directly.")
(put 'web-webview-url 'permanent-local t)

(defvar-local web-webview-revive-function nil
  "Function (a symbol) that recreates this view from scratch.
Used by session-reload when restoring across a restart: views whose
backing server ran inside the old Emacs (e.g. org-roam-ui) need the
server restarted, not just the buffer recreated.  Called with no
arguments; must be idempotent.  nil means recreating the buffer with
`web-webview-open' is enough.")
(put 'web-webview-revive-function 'permanent-local t)

(define-derived-mode web-webview-mode special-mode "WebView"
  "Major mode for buffers displayed as an embedded browser view.
The buffer text is only a placeholder; the web client draws an
iframe over the window body."
  (setq-local cursor-type nil))

(defun web-webview--placeholder (url)
  (let ((inhibit-read-only t))
    (erase-buffer)
    (insert "Embedded web view\n\n  " url "\n\n"
            "Rendered by the web display client; if you are reading\n"
            "this, the client has not drawn the overlay (yet).")
    (goto-char (point-min))))

;;;###autoload
(defun web-webview-open (url &optional buffer-name)
  "Display URL as an embedded browser view in a buffer.
Reuses the buffer named BUFFER-NAME (default \"*webview: URL*\")
if it exists.  Returns the buffer."
  (interactive "sURL: ")
  (unless (eq window-system 'web)
    (user-error "web-webview requires the web display backend"))
  (let ((buf (get-buffer-create (or buffer-name
                                    (format "*webview: %s*" url)))))
    (with-current-buffer buf
      (unless (derived-mode-p 'web-webview-mode)
        (web-webview-mode))
      (setq web-webview-url url)
      (web-webview--placeholder url))
    (pop-to-buffer buf)
    buf))

;;;###autoload
(defun web-org-roam-graph ()
  "Open the org-roam-ui graph as an embedded view in a buffer.
Starts the org-roam-ui server if it is not already running."
  (interactive)
  (require 'org-roam-ui)
  (defvar org-roam-ui-port)
  (defvar org-roam-ui-open-on-start)
  (defvar org-roam-ui-mode)
  ;; Don't let org-roam-ui call browse-url; the iframe IS the browser.
  (let ((org-roam-ui-open-on-start nil))
    (unless org-roam-ui-mode
      (org-roam-ui-mode 1)))
  (let ((buf (web-webview-open (format "http://localhost:%d"
                                       org-roam-ui-port)
                               "*org-roam-graph*")))
    (with-current-buffer buf
      (setq web-webview-revive-function #'web-org-roam-graph))
    buf))

(provide 'web-webview)

;;; web-webview.el ends here
