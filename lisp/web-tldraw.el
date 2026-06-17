;;; web-tldraw.el --- tldraw whiteboards as Emacs buffers (web backend) -*- lexical-binding: t -*-

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
;; can host a real tldraw whiteboard (https://tldraw.dev).  A buffer in
;; `tldraw-mode' whose buffer-local `web-tldraw-board-id' is set gets a
;; tldraw canvas overlaid on its window body by the client (see
;; web-client/src/tldraw/).  The board's *content* lives in the buffer
;; as a JSON snapshot — so a board is just a buffer: visit a `.tldr'
;; file, edit it, save it, diff it, restore it across hot reloads.
;;
;;   M-x web-tldraw-create RET                 ; a scratch board
;;   C-u M-x web-tldraw-create RET foo.tldr    ; a file-backed board
;;
;; Boards are keyboard-first.  Emacs owns the keyboard; the canvas is a
;; passive overlay (pointer-events:none) driven through the tldraw
;; Editor API.  In `tldraw-mode':
;;
;;   C-n C-p C-b C-f   move focus to the nearest node down/up/left/right
;;   C-M-n C-M-p       cycle focus through nodes in z-order
;;   RET               edit the selected node in an Emacs buffer
;;                     (or, with an edge anchor set, draw an edge)
;;   C-SPC             set the selected node as an edge anchor
;;   < >               bend the selected arrow
;;   a t               add a node / a text label
;;   d                 delete the selection
;;   = +               zoom to fit / center on selection
;;   m                 toggle mouse interaction on this board
;;   C-g               clear the edge anchor
;;
;; The data flows over a small `tldraw_*' wire-message family carried by
;; `web-tldraw-send' (C, webterm.c) outbound and an inbound queue drained
;; by a timer (`web-tldraw--take-pending', C).
;;
;; A public API (`web-tldraw-add-node', `web-tldraw-add-edge',
;; `web-tldraw-spytial', ...) lets packages render structures as boards.

;;; Code:

(require 'json)
(require 'seq)
(require 'cl-lib)
(require 'color)

(defgroup web-tldraw nil
  "Embed tldraw whiteboards in Emacs buffers via the web display."
  :group 'web)

;;;; Buffer-local board state

(defvar-local web-tldraw-board-id nil
  "Stable id of the tldraw board overlaid on this buffer's window.
Read by the C layer (webterm.c) during redisplay; nil means no board.
Set automatically by `tldraw-mode'.")
(put 'web-tldraw-board-id 'permanent-local t)

(defvar-local web-tldraw-epoch 0
  "Monotonic content epoch for this board.
Bumped each time Emacs sends an authoritative snapshot to the client
with `web-tldraw--send-load'; the client stamps the snapshots it sends
back with the epoch they descend from, so stale echoes are ignored.")
(put 'web-tldraw-epoch 'permanent-local t)

(defvar-local web-tldraw--selection nil
  "Id of the currently selected shape on this board (from the client).")
(defvar-local web-tldraw--selection-text nil
  "Text of the currently selected shape, for the mode line.")
(defvar-local web-tldraw--selection-meta nil
  "Alist of the currently selected shape's `meta' (from the client).")
(defvar-local web-tldraw--recenter-state 0
  "Cycle counter for `web-tldraw-recenter' (like `recenter-top-bottom').")
(defvar-local web-tldraw--edge-anchor nil
  "Shape id marked as the start of an edge being drawn, or nil.")
(defvar-local web-tldraw--region-count 0
  "Number of nodes in the current visual selection (for the mode line).")
(defvar-local web-tldraw--visual-style 'box
  "Style of the active visual selection: `box' (rectangle) or `path'
\(accumulate each node moved onto).")
(defvar-local web-tldraw--mouse nil
  "Non-nil when direct mouse interaction is enabled on this board.")
(defvar-local web-tldraw--ready nil
  "Non-nil once the client has mounted this board's canvas.")
(defvar-local web-tldraw--on-ready nil
  "Thunks to run when this board next becomes ready (newest first).")

(defvar web-tldraw-revive-function nil
  "If non-nil in a board buffer, a function to recreate it after reload.")
(make-variable-buffer-local 'web-tldraw-revive-function)
(put 'web-tldraw-revive-function 'permanent-local t)

;;;; Outbound helpers

(defun web-tldraw--connected-p ()
  "Return non-nil when a web display is available to send to."
  (and (eq window-system 'web) (fboundp 'web-tldraw-send)))

(defun web-tldraw--send (plist)
  "Serialize PLIST to JSON and send it to the client."
  (when (web-tldraw--connected-p)
    (web-tldraw-send (json-serialize plist))))

(defun web-tldraw--cmd (verb &optional args board)
  "Send command VERB (with ARGS plist) to BOARD (default current)."
  (let ((b (or board web-tldraw-board-id)))
    (when b
      (web-tldraw--send
       (list :type "tldraw_cmd" :board b :verb verb
             :args (or args (make-hash-table)))))))

;;;; UI customization (buffer-local, falling back to global defaults)

(defcustom web-tldraw-hide-ui t
  "Whether to hide tldraw's on-canvas UI (toolbars, menus, panels).
Defaults to t: boards are keyboard-driven, so the mouse UI is hidden and
only revealed in mouse mode (\\[web-tldraw-toggle-mouse]).  Set
buffer-locally or toggle with \\[web-tldraw-toggle-ui] to override."
  :type 'boolean :group 'web-tldraw)

(defcustom web-tldraw-grid nil
  "Whether tldraw boards show the background grid.
Global default; buffer-locally overridable like `web-tldraw-hide-ui'."
  :type 'boolean :group 'web-tldraw)

(defun web-tldraw--send-config (&optional board)
  "Push this board's UI preferences to the client."
  (let ((b (or board web-tldraw-board-id)))
    (when b
      (web-tldraw--send
       (list :type "tldraw_config" :board b
             :ui (list :hideUi (if web-tldraw-hide-ui t :false)
                       :grid (if web-tldraw-grid t :false)))))))

(defun web-tldraw-toggle-ui ()
  "Toggle the tldraw on-canvas UI for this board."
  (interactive)
  (setq-local web-tldraw-hide-ui (not web-tldraw-hide-ui))
  (web-tldraw--send-config)
  (message "tldraw UI: %s" (if web-tldraw-hide-ui "hidden" "shown")))

(defun web-tldraw-toggle-grid ()
  "Toggle the tldraw background grid for this board."
  (interactive)
  (setq-local web-tldraw-grid (not web-tldraw-grid))
  (web-tldraw--send-config)
  (message "tldraw grid: %s" (if web-tldraw-grid "on" "off")))

;;;; Snapshot sync (Emacs buffer <-> client board)

(defun web-tldraw--make-id ()
  "Make a stable board id for the current buffer.
File buffers hash their truename so the same file maps to the same
board across restarts; scratch buffers get a random id."
  (secure-hash 'md5 (or buffer-file-truename
                        (format "scratch:%s:%d" (buffer-name) (random)))))

(defun web-tldraw--valid-snapshot (text)
  "Return TEXT if it is valid JSON, else nil (treated as an empty board)."
  (and (not (string-blank-p text))
       (when (ignore-errors (and (json-parse-string text) t)) text)))

(defun web-tldraw--send-load ()
  "Send the current buffer's snapshot to the client as authoritative.
Bumps `web-tldraw-epoch'.  The buffer text is embedded verbatim as the
snapshot (it is the tldraw document JSON); a blank/invalid buffer loads
an empty board."
  (when (and web-tldraw-board-id (web-tldraw--connected-p))
    (setq web-tldraw-epoch (1+ web-tldraw-epoch))
    (let* ((text (buffer-substring-no-properties (point-min) (point-max)))
           (snap (web-tldraw--valid-snapshot text)))
      ;; Build by hand so the snapshot JSON is embedded, not re-escaped.
      (web-tldraw-send
       (format "{\"type\":\"tldraw_load\",\"board\":\"%s\",\"epoch\":%d,\"snapshot\":%s}"
               web-tldraw-board-id web-tldraw-epoch (or snap "null"))))))

(defun web-tldraw--handle-snapshot (msg)
  "Persist a client snapshot MSG into its board buffer.
Accepts only snapshots stamped with the current epoch (ignores stale
ones from before an in-flight load).  Writing the buffer does not
trigger a re-load, so there is no echo loop."
  (let ((buf (web-tldraw--buffer-for-board (alist-get 'board msg)))
        (epoch (or (alist-get 'epoch msg) 0))
        (snap (alist-get 'snapshot msg)))
    (when (and buf snap)
      (with-current-buffer buf
        (when (>= epoch web-tldraw-epoch)
          (let ((json (json-serialize snap))
                (inhibit-read-only t))
            (unless (string= json (buffer-substring-no-properties
                                   (point-min) (point-max)))
              (let ((pt (point)))
                (erase-buffer)
                (insert json)
                (goto-char (min pt (point-max))))
              (set-buffer-modified-p t))))))))

;;;; Inbound event queue (drained by a timer)

(defvar web-tldraw--timer nil
  "Repeating timer draining inbound tldraw events while boards exist.")

(defun web-tldraw--any-boards-p ()
  "Return non-nil if any live buffer is a tldraw board."
  (seq-some (lambda (b) (buffer-local-value 'web-tldraw-board-id b))
            (buffer-list)))

(defun web-tldraw--ensure-timer ()
  "Start the inbound-drain timer if it is not already running."
  (unless (and web-tldraw--timer (memq web-tldraw--timer timer-list))
    (setq web-tldraw--timer
          (run-with-timer 0.1 0.1 #'web-tldraw--drain))))

(defun web-tldraw--gc-timer ()
  "Stop the drain timer once no board buffers remain (keeps idle CPU 0)."
  (unless (web-tldraw--any-boards-p)
    (when web-tldraw--timer
      (cancel-timer web-tldraw--timer)
      (setq web-tldraw--timer nil))))

(defun web-tldraw--drain ()
  "Drain and dispatch queued inbound `tldraw_*' messages."
  (when (fboundp 'web-tldraw--take-pending)
    (dolist (line (web-tldraw--take-pending))
      (condition-case err
          (web-tldraw--dispatch line)
        (error (message "web-tldraw: dispatch error: %S" err))))))

(defun web-tldraw--buffer-for-board (board)
  "Return the live buffer whose board id is BOARD, or nil."
  (and board
       (seq-find (lambda (b)
                   (equal (buffer-local-value 'web-tldraw-board-id b) board))
                 (buffer-list))))

(defun web-tldraw--dispatch (line)
  "Parse one inbound JSON LINE and route it."
  (let* ((msg (json-parse-string line :object-type 'alist))
         (type (alist-get 'type msg)))
    (cond
     ((equal type "tldraw_snapshot") (web-tldraw--handle-snapshot msg))
     ((equal type "tldraw_event") (web-tldraw--handle-event msg)))))

(defun web-tldraw--handle-event (msg)
  "Handle a `tldraw_event' MSG from the client."
  (let ((buf (web-tldraw--buffer-for-board (alist-get 'board msg)))
        (event (alist-get 'event msg)))
    (when buf
      (with-current-buffer buf
        (pcase event
          ("ready"
           (web-tldraw--send-load)
           (web-tldraw-sync-theme)
           (web-tldraw--send-config)
           (setq web-tldraw--ready t)
           (let ((thunks (nreverse web-tldraw--on-ready)))
             (setq web-tldraw--on-ready nil)
             (dolist (fn thunks) (ignore-errors (funcall fn)))))
          ("selection"
           (setq web-tldraw--selection (alist-get 'id msg)
                 web-tldraw--selection-text (alist-get 'text msg)
                 web-tldraw--selection-meta (alist-get 'meta msg)
                 web-tldraw--recenter-state 0)
           (force-mode-line-update))
          ("created"
           (setq web-tldraw--selection (alist-get 'id msg)
                 web-tldraw--selection-meta (alist-get 'meta msg)))
          ("region"
           (setq web-tldraw--region-count (or (alist-get 'count msg) 0))
           (force-mode-line-update))
          ("edit-begin" (web-tldraw--open-node-editor msg))
          ("edit-end" (setq web-tldraw--selection (alist-get 'id msg))))))))

;;;; Major mode

(defvar tldraw-mode-map (make-sparse-keymap)
  "Keymap for `tldraw-mode' (keyboard-driven whiteboard).")

;; `define-keymap' with :keymap mutates the existing map in place, so a
;; plain `load-file' of this file (re-evaluating this form) updates the
;; bindings of already-open `tldraw-mode' buffers — unlike
;; `defvar-keymap', whose `defvar' would not reassign on reload.
(define-keymap :keymap tldraw-mode-map
  "C-n"   #'web-tldraw-focus-down
  "C-p"   #'web-tldraw-focus-up
  "C-f"   #'web-tldraw-focus-right
  "C-b"   #'web-tldraw-focus-left
  "n"     #'web-tldraw-focus-down
  "p"     #'web-tldraw-focus-up
  "f"     #'web-tldraw-focus-right
  "b"     #'web-tldraw-focus-left
  "C-M-n" #'web-tldraw-focus-next
  "C-M-p" #'web-tldraw-focus-prev
  "TAB"   #'web-tldraw-focus-next
  "<backtab>" #'web-tldraw-focus-prev
  "RET"   #'web-tldraw-ret
  "C-SPC" #'web-tldraw-edge-mark
  "v"     #'web-tldraw-visual
  "V"     #'web-tldraw-visual-path
  "C-g"   #'web-tldraw-quit
  "C-l"   #'web-tldraw-recenter
  "M-l"   #'web-tldraw-recenter
  "a"     #'web-tldraw-add-node-interactive
  "t"     #'web-tldraw-add-text-interactive
  "e"     #'web-tldraw-enter-edit
  "d"     #'web-tldraw-delete
  "<"     #'web-tldraw-bend-left
  ">"     #'web-tldraw-bend-right
  "|"     #'web-tldraw-bend-reset
  "="     #'web-tldraw-zoom-fit
  "+"     #'web-tldraw-zoom-in
  "-"     #'web-tldraw-zoom-out
  "0"     #'web-tldraw-zoom-reset
  "."     #'web-tldraw-center
  "C-/"   #'web-tldraw-undo
  "C-_"   #'web-tldraw-undo
  "C-?"   #'web-tldraw-redo
  "u"     #'web-tldraw-toggle-ui
  ;; Move the selected node: plain arrows (or M-arrows) by a step,
  ;; Shift-arrows by a larger step.
  "<up>"      #'web-tldraw-nudge-up
  "<down>"    #'web-tldraw-nudge-down
  "<left>"    #'web-tldraw-nudge-left
  "<right>"   #'web-tldraw-nudge-right
  "M-<up>"    #'web-tldraw-nudge-up
  "M-<down>"  #'web-tldraw-nudge-down
  "M-<left>"  #'web-tldraw-nudge-left
  "M-<right>" #'web-tldraw-nudge-right
  "S-<up>"    #'web-tldraw-nudge-up-big
  "S-<down>"  #'web-tldraw-nudge-down-big
  "S-<left>"  #'web-tldraw-nudge-left-big
  "S-<right>" #'web-tldraw-nudge-right-big
  "m"     #'web-tldraw-toggle-mouse)

(defun web-tldraw--mode-line ()
  "Mode-line tail for a tldraw board: input mode, edge state, selection."
  (concat
   (if (bound-and-true-p web-tldraw--mouse) " [mouse]" " [kbd]")
   (when (bound-and-true-p web-tldraw-visual-mode)
     (format " [%s %d]" web-tldraw--visual-style web-tldraw--region-count))
   (when web-tldraw--edge-anchor " [edge]")
   (when (and (stringp web-tldraw--selection-text)
              (not (string-empty-p web-tldraw--selection-text)))
     (format " {%s}"
             (truncate-string-to-width
              (car (split-string web-tldraw--selection-text "\n"))
              20 nil nil "…")))))

(define-derived-mode tldraw-mode special-mode "Tldraw"
  "Major mode for tldraw whiteboard buffers.
The buffer text is the board's JSON snapshot (so it saves, diffs, and
version-controls like any file); the web client draws the live canvas
over the window body.  The buffer is read-only to keystrokes — editing
happens through the canvas and the navigation commands — but snapshots
written by the client and programmatic edits update it."
  (setq-local web-tldraw-board-id (web-tldraw--make-id))
  (setq-local cursor-type nil)
  (setq-local truncate-lines t)
  (setq-local web-tldraw-epoch 0)
  (setq-local mode-line-process
              '(:eval (web-tldraw--mode-line)))
  (web-tldraw--ensure-timer)
  (add-hook 'after-revert-hook #'web-tldraw--send-load nil t)
  (add-hook 'kill-buffer-hook
            (lambda () (run-with-timer 0.5 nil #'web-tldraw--gc-timer))
            nil t))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.tldr\\'" . tldraw-mode))

;;;###autoload
(defun web-tldraw-create (&optional file)
  "Create or visit a tldraw whiteboard buffer.
With a prefix argument (or non-nil FILE), prompt for / visit a `.tldr'
file; otherwise create a scratch board.  Returns the buffer."
  (interactive
   (list (when current-prefix-arg
           (read-file-name "tldraw file: " nil "board.tldr"))))
  (let ((buf (if file
                 (find-file-noselect file)
               (generate-new-buffer "*tldraw*"))))
    (with-current-buffer buf
      (unless (derived-mode-p 'tldraw-mode) (tldraw-mode)))
    (pop-to-buffer buf)
    buf))

;;;; Navigation commands

(defun web-tldraw-focus-down ()  (interactive) (web-tldraw--cmd "focus-move" '(:dir "down")))
(defun web-tldraw-focus-up ()    (interactive) (web-tldraw--cmd "focus-move" '(:dir "up")))
(defun web-tldraw-focus-left ()  (interactive) (web-tldraw--cmd "focus-move" '(:dir "left")))
(defun web-tldraw-focus-right () (interactive) (web-tldraw--cmd "focus-move" '(:dir "right")))
(defun web-tldraw-focus-next ()  (interactive) (web-tldraw--cmd "focus-seq"  '(:dir "next")))
(defun web-tldraw-focus-prev ()  (interactive) (web-tldraw--cmd "focus-seq"  '(:dir "prev")))
(defun web-tldraw-zoom-fit ()    (interactive) (web-tldraw--cmd "camera" '(:action "fit")))
(defun web-tldraw-zoom-in ()     (interactive) (web-tldraw--cmd "camera" '(:action "in")))
(defun web-tldraw-zoom-out ()    (interactive) (web-tldraw--cmd "camera" '(:action "out")))
(defun web-tldraw-zoom-reset ()  (interactive) (web-tldraw--cmd "camera" '(:action "reset")))
(defun web-tldraw-center ()      (interactive) (web-tldraw--cmd "camera" '(:action "center")))
(defun web-tldraw-undo ()        (interactive) (web-tldraw--cmd "undo"))
(defun web-tldraw-redo ()        (interactive) (web-tldraw--cmd "redo"))
(defun web-tldraw-bend-left ()   (interactive) (web-tldraw--cmd "nudge-bend" '(:delta -30)))
(defun web-tldraw-bend-right ()  (interactive) (web-tldraw--cmd "nudge-bend" '(:delta 30)))
(defun web-tldraw-bend-reset ()
  "Straighten the selected arrow (reset its bend/control point)."
  (interactive) (web-tldraw--cmd "nudge-bend" '(:reset t)))
(defcustom web-tldraw-nudge-step 20
  "Pixels the selected node moves per `web-tldraw-nudge-*' command.
Shift-arrow variants move `web-tldraw-nudge-big-step' pixels."
  :type 'integer :group 'web-tldraw)

(defcustom web-tldraw-nudge-big-step 100
  "Pixels the selected node moves for the Shift-arrow nudge variants."
  :type 'integer :group 'web-tldraw)

(defun web-tldraw-nudge (dx dy)
  "Move the selected node by DX,DY pixels."
  (web-tldraw--cmd "nudge-shape" (list :dx dx :dy dy)))

(defun web-tldraw-nudge-up ()    (interactive) (web-tldraw-nudge 0 (- web-tldraw-nudge-step)))
(defun web-tldraw-nudge-down ()  (interactive) (web-tldraw-nudge 0 web-tldraw-nudge-step))
(defun web-tldraw-nudge-left ()  (interactive) (web-tldraw-nudge (- web-tldraw-nudge-step) 0))
(defun web-tldraw-nudge-right () (interactive) (web-tldraw-nudge web-tldraw-nudge-step 0))
(defun web-tldraw-nudge-up-big ()    (interactive) (web-tldraw-nudge 0 (- web-tldraw-nudge-big-step)))
(defun web-tldraw-nudge-down-big ()  (interactive) (web-tldraw-nudge 0 web-tldraw-nudge-big-step))
(defun web-tldraw-nudge-left-big ()  (interactive) (web-tldraw-nudge (- web-tldraw-nudge-big-step) 0))
(defun web-tldraw-nudge-right-big () (interactive) (web-tldraw-nudge web-tldraw-nudge-big-step 0))

(defun web-tldraw-recenter ()
  "Cycle the selected node through center/top/bottom of the viewport.
Like `recenter-top-bottom' (\\[recenter-top-bottom]): repeated calls
cycle the camera so the node sits centered, then near the top, then
near the bottom."
  (interactive)
  (let* ((states ["center" "top" "bottom"])
         (pos (aref states (mod web-tldraw--recenter-state 3))))
    (setq web-tldraw--recenter-state (1+ web-tldraw--recenter-state))
    (web-tldraw--cmd "recenter" (list :pos pos))))

(defun web-tldraw-delete ()
  "Delete the selected shape(s)."
  (interactive)
  (web-tldraw--cmd "delete"))

(defun web-tldraw-toggle-mouse ()
  "Toggle direct mouse interaction on this board (default off)."
  (interactive)
  (setq-local web-tldraw--mouse (not (bound-and-true-p web-tldraw--mouse)))
  (web-tldraw--cmd "mouse" (list :on (if web-tldraw--mouse t :false)))
  (force-mode-line-update)
  (message "tldraw mouse: %s (UI %s)"
           (if web-tldraw--mouse "on" "off")
           (if web-tldraw--mouse "shown" "hidden")))

(defun web-tldraw-enter-edit ()
  "Edit the selected node's text in an Emacs buffer.
Sends `enter-edit'; the client replies with the node's current text
(an `edit-begin' event), which opens the edit buffer."
  (interactive)
  (web-tldraw--cmd "enter-edit"))

(defvar web-tldraw-node-action-functions nil
  "Abnormal hook run by RET on the selected node.
Each function is called with (BOARD NODE-ID META TEXT), where META is an
alist of the shape's `meta' and TEXT its label.  The first function to
return non-nil handles the node (run via
`run-hook-with-args-until-success'); a function may create new nodes,
open files, unfold a tree, etc.  When no function handles the node, RET
falls back to editing it (`web-tldraw-enter-edit').  Add buffer-local
entries for per-board behavior (see `web-tldraw-dired').")

(defun web-tldraw-node-meta (key &optional default)
  "Return KEY from the selected node's `meta' alist, or DEFAULT."
  (or (alist-get key web-tldraw--selection-meta) default))

(defun web-tldraw-ret ()
  "Act on the selected node.
With an edge anchor set, draw an edge to the selection; otherwise run
`web-tldraw-node-action-functions', falling back to editing the node."
  (interactive)
  (cond
   (web-tldraw--edge-anchor (web-tldraw-edge-commit))
   ((run-hook-with-args-until-success
     'web-tldraw-node-action-functions
     web-tldraw-board-id web-tldraw--selection
     web-tldraw--selection-meta web-tldraw--selection-text))
   (t (web-tldraw-enter-edit))))

(defun web-tldraw-quit ()
  "Clear the edge anchor; otherwise behave like `keyboard-quit'."
  (interactive)
  (if web-tldraw--edge-anchor
      (progn (setq web-tldraw--edge-anchor nil)
             (force-mode-line-update)
             (message "tldraw: edge anchor cleared"))
    (keyboard-quit)))

;;;; Edge drawing

(defun web-tldraw-edge-mark ()
  "Mark the selected node as the start of a new edge."
  (interactive)
  (if web-tldraw--selection
      (progn
        (setq web-tldraw--edge-anchor web-tldraw--selection)
        (force-mode-line-update)
        (message "tldraw: edge anchor set; navigate to a target and RET"))
    (message "tldraw: select a node first")))

(defun web-tldraw-edge-commit ()
  "Connect the edge anchor to the current selection with an arrow."
  (interactive)
  (if (and web-tldraw--edge-anchor web-tldraw--selection
           (not (equal web-tldraw--edge-anchor web-tldraw--selection)))
      (progn
        (web-tldraw--cmd "create-edge"
                         (list :from web-tldraw--edge-anchor
                               :to web-tldraw--selection))
        (setq web-tldraw--edge-anchor nil)
        (force-mode-line-update)
        (message "tldraw: edge drawn"))
    (message "tldraw: need an anchor and a different target node")))

;;;; Visual box-selection mode

(defvar tldraw-visual-mode-map (make-sparse-keymap)
  "Keymap active in `web-tldraw-visual-mode'.")
(define-keymap :keymap tldraw-visual-mode-map
  "C-n" #'web-tldraw-visual-down   "n" #'web-tldraw-visual-down
  "C-p" #'web-tldraw-visual-up     "p" #'web-tldraw-visual-up
  "C-f" #'web-tldraw-visual-right  "f" #'web-tldraw-visual-right
  "C-b" #'web-tldraw-visual-left   "b" #'web-tldraw-visual-left
  "<down>"  #'web-tldraw-visual-down
  "<up>"    #'web-tldraw-visual-up
  "<right>" #'web-tldraw-visual-right
  "<left>"  #'web-tldraw-visual-left
  "RET" #'web-tldraw-visual-accept
  "d"   #'web-tldraw-visual-delete
  "C-g" #'web-tldraw-visual-cancel
  "q"   #'web-tldraw-visual-cancel)

(define-minor-mode web-tldraw-visual-mode
  "Keyboard box-selection mode for a tldraw board.
Entered at a node (the anchor); the movement keys grow a rectangle to a
cursor node and select every node inside it (the count shows in the mode
line).  \\<tldraw-visual-mode-map>\\[web-tldraw-visual-accept] keeps the
selection, \\[web-tldraw-visual-delete] deletes it, \\[web-tldraw-visual-cancel]
cancels.  With a multi-selection the move keys (in normal mode) shift the
whole group."
  :lighter nil
  (if web-tldraw-visual-mode
      (web-tldraw--cmd "visual-start"
                       (list :style (symbol-name web-tldraw--visual-style)))
    (web-tldraw--cmd "visual-end"))
  (force-mode-line-update))

(defun web-tldraw-visual ()
  "Enter visual BOX-selection mode at the current node.
Movement grows a rectangle; every node inside it is selected."
  (interactive)
  (setq-local web-tldraw--visual-style 'box)
  (web-tldraw-visual-mode 1)
  (message "tldraw: box-select — move to grow, RET keep, d delete, C-g cancel"))

(defun web-tldraw-visual-path ()
  "Enter visual PATH-selection mode at the current node.
Each node you move onto is added to the selection (a freeform set,
rather than a rectangle)."
  (interactive)
  (setq-local web-tldraw--visual-style 'path)
  (web-tldraw-visual-mode 1)
  (message "tldraw: path-select — move to add nodes, RET keep, d delete, C-g cancel"))

(defun web-tldraw-visual-down ()  (interactive) (web-tldraw--cmd "visual-move" '(:dir "down")))
(defun web-tldraw-visual-up ()    (interactive) (web-tldraw--cmd "visual-move" '(:dir "up")))
(defun web-tldraw-visual-left ()  (interactive) (web-tldraw--cmd "visual-move" '(:dir "left")))
(defun web-tldraw-visual-right () (interactive) (web-tldraw--cmd "visual-move" '(:dir "right")))

(defun web-tldraw-visual-accept ()
  "Exit visual mode, keeping the box selection."
  (interactive)
  (web-tldraw-visual-mode -1)
  (message "tldraw: %d node(s) selected" web-tldraw--region-count))

(defun web-tldraw-visual-delete ()
  "Delete the box selection and exit visual mode."
  (interactive)
  (web-tldraw--cmd "delete")
  (web-tldraw-visual-mode -1))

(defun web-tldraw-visual-cancel ()
  "Clear the box selection and exit visual mode."
  (interactive)
  (web-tldraw--cmd "select" (list :ids []))
  (web-tldraw-visual-mode -1)
  (message "tldraw: selection cleared"))

;;;; In-node editing (Emacs-routed)

(defvar-local web-tldraw--node-board nil)
(defvar-local web-tldraw--node-id nil)
(defvar-local web-tldraw--node-timer nil)

(defcustom web-tldraw-node-comment-start "# "
  "Prefix marking comment lines in the node-edit buffer.
Like Git commit messages, lines beginning with this prefix are
informational and are stripped from the text sent to the node."
  :type 'string :group 'web-tldraw)

(defvar web-tldraw-node-edit-mode-map (make-sparse-keymap)
  "Keymap while editing a tldraw node in an Emacs buffer.")
(define-keymap :keymap web-tldraw-node-edit-mode-map
  "C-c C-c" #'web-tldraw-node-commit
  "C-c C-k" #'web-tldraw-node-cancel)

(define-minor-mode web-tldraw-node-edit-mode
  "Minor mode for the buffer that edits a tldraw node's text.
Edits mirror live onto the node; \\[web-tldraw-node-commit] commits and
returns to the board, \\[web-tldraw-node-cancel] cancels.  Lines starting
with `web-tldraw-node-comment-start' are ignored (like a Git commit)."
  :lighter " TldrawNode")

(defun web-tldraw--node-comment-re ()
  "Regexp matching a comment line in the node-edit buffer."
  (concat "\\`[ \t]*"
          (regexp-quote (string-trim-right web-tldraw-node-comment-start))))

(defun web-tldraw--node-insert-comments (id board stype)
  "Insert the Git-commit-style help/context comment block."
  (let ((cs web-tldraw-node-comment-start)
        (beg (point)))
    (insert "\n"
            cs "Edit the node's text above; it mirrors onto the canvas live.\n"
            cs "\n"
            cs (substitute-command-keys
                "\\<web-tldraw-node-edit-mode-map>\\[web-tldraw-node-commit] commit and return to the board   \\[web-tldraw-node-cancel] cancel\n")
            cs "Lines starting with \"" (string-trim-right cs)
            "\" are ignored.\n"
            cs "\n"
            cs (format "board: %s\n" (substring board 0 (min 8 (length board))))
            cs (format "node:  %s%s\n" id
                       (if (or (null stype) (string-empty-p stype)) ""
                         (format " (%s)" stype))))
    (add-text-properties beg (point)
                         '(face font-lock-comment-face rear-nonsticky t))))

(defun web-tldraw--open-node-editor (msg)
  "Open an Emacs buffer to edit the node described by MSG (edit-begin).
Called with the board buffer current."
  (let* ((board web-tldraw-board-id)
         (id (alist-get 'id msg))
         (text (or (alist-get 'text msg) ""))
         (stype (alist-get 'shapeType msg))
         (name (format "*tldraw-node: %s*"
                       (substring id 0 (min 8 (length id)))))
         (ebuf (get-buffer-create name)))
    (with-current-buffer ebuf
      (text-mode)
      (web-tldraw-node-edit-mode 1)
      (setq web-tldraw--node-board board
            web-tldraw--node-id id)
      (erase-buffer)
      (insert text)
      (unless (bolp) (insert "\n"))
      (let ((text-end (point)))
        (web-tldraw--node-insert-comments id board stype)
        (set-buffer-modified-p nil)
        (goto-char (min text-end (point-max))))
      (add-hook 'after-change-functions
                #'web-tldraw--node-after-change nil t))
    (pop-to-buffer ebuf)
    (message "tldraw: editing node; C-c C-c to commit, C-c C-k to cancel")))

(defun web-tldraw--node-content ()
  "Return the node text from the edit buffer, minus comment lines."
  (let ((re (web-tldraw--node-comment-re))
        (lines '()))
    (save-excursion
      (goto-char (point-min))
      (while (not (eobp))
        (let ((line (buffer-substring-no-properties
                     (line-beginning-position) (line-end-position))))
          (unless (string-match-p re line) (push line lines)))
        (forward-line 1)))
    (string-trim (string-join (nreverse lines) "\n"))))

(defun web-tldraw--node-after-change (&rest _)
  "Debounce-mirror this node-edit buffer's text onto the canvas."
  (when web-tldraw--node-timer (cancel-timer web-tldraw--node-timer))
  (setq web-tldraw--node-timer
        (run-with-timer 0.15 nil #'web-tldraw--node-flush
                        (current-buffer))))

(defun web-tldraw--node-flush (buf)
  "Send BUF's current text (sans comments) to its node on the canvas."
  (when (buffer-live-p buf)
    (with-current-buffer buf
      (web-tldraw--send
       (list :type "tldraw_node_text"
             :board web-tldraw--node-board
             :id web-tldraw--node-id
             :text (web-tldraw--node-content))))))

(defun web-tldraw-node-commit ()
  "Commit the node edit and return focus to the board."
  (interactive)
  (web-tldraw--node-flush (current-buffer))
  (web-tldraw--cmd "exit-edit" (list :id web-tldraw--node-id)
                   web-tldraw--node-board)
  (let ((board web-tldraw--node-board))
    (quit-window)
    (web-tldraw--select-board-window board)))

(defun web-tldraw-node-cancel ()
  "Abandon the node edit (the last mirrored text stays on the canvas)."
  (interactive)
  (web-tldraw--cmd "exit-edit" (list :id web-tldraw--node-id)
                   web-tldraw--node-board)
  (let ((board web-tldraw--node-board))
    (quit-window)
    (web-tldraw--select-board-window board)))

(defun web-tldraw--select-board-window (board)
  "Select a window showing BOARD, if any."
  (let ((buf (web-tldraw--buffer-for-board board)))
    (when buf
      (let ((win (get-buffer-window buf)))
        (when win (select-window win))))))

;;;; Theme sync

(defun web-tldraw--color-luminance (hex)
  "Return the 0..1 relative luminance of HEX color, or 0.5 if unknown."
  (if (and (stringp hex) (color-defined-p hex))
      (pcase-let ((`(,r ,g ,b) (color-name-to-rgb hex)))
        (+ (* 0.2126 r) (* 0.7152 g) (* 0.0722 b)))
    0.5))

(defun web-tldraw--hex (color)
  "Return COLOR as a #rrggbb hex string, or nil."
  (when (and (stringp color) (color-defined-p color))
    (apply #'color-rgb-to-hex (append (color-name-to-rgb color) '(2)))))

(defcustom web-tldraw-font-family nil
  "Font family for tldraw board text, or nil to inherit the editor font.
A board-scoped override: boards normally use the same font as the editor
(via a shared CSS variable), so they always match; set this only to make
boards use a different font.  It does not affect the editor's own font or
metrics.  Apply a change to live boards with `web-tldraw-set-font'."
  :type '(choice (const :tag "Editor font" nil) (string :tag "Family"))
  :group 'web-tldraw)

(defun web-tldraw-set-font (family)
  "Set the tldraw board font to FAMILY for all boards.
Empty input reverts boards to the editor font.  This is a board-only
override and never changes the editor font/metrics."
  (interactive "sBoard font family (empty = editor font): ")
  (setq web-tldraw-font-family (if (string-empty-p family) nil family))
  (dolist (b (buffer-list))
    (with-current-buffer b
      (when web-tldraw-board-id (web-tldraw-sync-theme))))
  (message "tldraw board font: %s" (or web-tldraw-font-family "editor font")))

(defun web-tldraw-sync-theme (&rest _)
  "Push the active Emacs theme's colors (and board font) to every board.
Sends a broadcast `tldraw_theme' message: a dark/light color scheme
(from the frame background luminance), the canvas background, a palette
mapping tldraw's named colors onto theme faces, and the board font
override (`web-tldraw-font-family'; nil = inherit the editor font)."
  (when (web-tldraw--connected-p)
    (let* ((bg (web-tldraw--hex (face-background 'default nil t)))
           (fg (web-tldraw--hex (face-foreground 'default nil t)))
           (dark (< (web-tldraw--color-luminance bg) 0.5))
           (palette
            (list
             :blue   (web-tldraw--hex (face-foreground 'font-lock-keyword-face nil t))
             :green  (web-tldraw--hex (face-foreground 'font-lock-string-face nil t))
             :red    (web-tldraw--hex (face-foreground 'error nil t))
             :orange (web-tldraw--hex (face-foreground 'warning nil t))
             :violet (web-tldraw--hex (face-foreground 'font-lock-function-name-face nil t))
             :grey   (web-tldraw--hex (face-foreground 'shadow nil t)))))
      (web-tldraw--send
       (list :type "tldraw_theme"
             :colorScheme (if dark "dark" "light")
             :bg (or bg :null)
             :fg (or fg :null)
             :font (or web-tldraw-font-family :null)
             :palette palette)))))

;;;; Public API — render structures as boards

(defun web-tldraw-when-ready (fn &optional board)
  "Run FN when BOARD's canvas is ready (now if already ready).
FN runs with the board buffer current.  Use this to batch-build a board
right after `web-tldraw-create', since the canvas mounts asynchronously."
  (let ((buf (if board (web-tldraw--buffer-for-board board) (current-buffer))))
    (when (buffer-live-p buf)
      (with-current-buffer buf
        (if web-tldraw--ready
            (funcall fn)
          (push fn web-tldraw--on-ready))))))

(cl-defun web-tldraw-add-node (text &key x y w h size font board id quiet meta)
  "Add a node labelled TEXT to BOARD.
Keyword args: X Y canvas position, W H box size, SIZE text size
(\"s\"/\"m\"/\"l\"/\"xl\"), FONT (\"sans\"/\"mono\"/\"serif\"/\"draw\"),
ID an explicit shape id (so edges can reference it), QUIET to not
select/center the new node, META a plist of node metadata (readable by
`web-tldraw-node-action-functions')."
  (web-tldraw--cmd "create-node"
                   (append (list :text text :x (or x 0) :y (or y 0))
                           (when w (list :w w))
                           (when h (list :h h))
                           (when size (list :size size))
                           (when font (list :font font))
                           (when id (list :id id))
                           (when quiet (list :quiet t))
                           (when meta (list :meta meta)))
                   board))

(defun web-tldraw-set-meta (id meta &optional board)
  "Merge plist META into shape ID's metadata on BOARD."
  (web-tldraw--cmd "set-meta" (list :id id :meta meta) board))

(defun web-tldraw-add-text (text &optional x y board)
  "Add a free TEXT label at X,Y to BOARD."
  (web-tldraw--cmd "create-text"
                   (list :text text :x (or x 0) :y (or y 0)) board))

(defun web-tldraw-add-edge (from to &optional board)
  "Connect shape FROM to shape TO with an arrow on BOARD."
  (web-tldraw--cmd "create-edge" (list :from from :to to) board))

(defun web-tldraw-select (ids &optional board)
  "Select shapes IDS (a list) on BOARD."
  (web-tldraw--cmd "select" (list :ids (vconcat ids)) board))

(defun web-tldraw-add-node-interactive (text)
  "Prompt for TEXT and add a node near the current selection."
  (interactive "sNode text: ")
  (web-tldraw-add-node text))

(defun web-tldraw-add-text-interactive (text)
  "Prompt for TEXT and add a free label."
  (interactive "sLabel text: ")
  (web-tldraw-add-text text 0 0))

(defcustom web-tldraw-image-extensions
  '("png" "jpg" "jpeg" "gif" "svg" "webp" "bmp" "ico")
  "File extensions embedded as images (rather than opened) on a board."
  :type '(repeat string) :group 'web-tldraw)

(defun web-tldraw-image-file-p (path)
  "Return non-nil if PATH names an image (by `web-tldraw-image-extensions')."
  (member (downcase (or (file-name-extension path) ""))
          web-tldraw-image-extensions))

(defun web-tldraw--image-mime (path)
  "Best-guess MIME type for image PATH."
  (pcase (downcase (or (file-name-extension path) ""))
    ("png" "image/png") ("gif" "image/gif") ("webp" "image/webp")
    ((or "jpg" "jpeg") "image/jpeg") ("svg" "image/svg+xml")
    ("bmp" "image/bmp") ("ico" "image/x-icon") (_ "image/png")))

(cl-defun web-tldraw-add-image (path &key x y board id quiet meta (maxw 360))
  "Embed the image at PATH on BOARD as a tldraw image shape.
The file is read, base64-encoded, and sent as a data URL; the client
sizes it from the image's natural dimensions, capped at MAXW pixels."
  (when (file-readable-p path)
    (let* ((mime (web-tldraw--image-mime path))
           (data (with-temp-buffer
                   (set-buffer-multibyte nil)
                   (insert-file-contents-literally path)
                   (base64-encode-string (buffer-string) t)))
           (src (format "data:%s;base64,%s" mime data)))
      (web-tldraw--cmd
       "create-image"
       (append (list :src src :x (or x 0) :y (or y 0) :maxw maxw
                     :mime mime :name (file-name-nondirectory path))
               (when id (list :id id))
               (when quiet (list :quiet t))
               (when meta (list :meta meta)))
       board))))

;;;###autoload
(defun web-tldraw-filesystem (dir &optional max-depth)
  "Render the directory tree under DIR as a tldraw board.
Each file/directory becomes a node; containment becomes an arrow.  A
demonstration of the public API: it builds the board with explicit node
ids and positions (depth -> column, sibling index -> row), so it needs
no external layout engine.  MAX-DEPTH defaults to 3."
  (interactive "DDirectory: \nP")
  (let* ((max-depth (if (numberp max-depth) max-depth 3))
         (board-buf (web-tldraw-create))
         (board (buffer-local-value 'web-tldraw-board-id board-buf))
         (counter 0)
         (col-w 240) (row-h 90)
         (rows-at (make-hash-table :test 'eq)) ; depth -> next row
         (nodes '()) (edges '()))
    (cl-labels
        ((walk (path depth parent-id)
           (when (<= depth max-depth)
             (let* ((id (format "shape:fs%d" (cl-incf counter)))
                    (row (or (gethash depth rows-at) 0))
                    (name (file-name-nondirectory (directory-file-name path)))
                    (dirp (file-directory-p path)))
               (puthash depth (1+ row) rows-at)
               (push (list :id id
                           :text (if dirp (concat name "/") name)
                           :x (* depth col-w) :y (* row row-h))
                     nodes)
               (when parent-id (push (cons parent-id id) edges))
               (when (and dirp (< depth max-depth))
                 (dolist (child (ignore-errors
                                  (directory-files path t
                                                   directory-files-no-dot-files-regexp)))
                   (walk child (1+ depth) id)))))))
      (walk (expand-file-name dir) 0 nil))
    (web-tldraw-when-ready
     (lambda ()
       (dolist (n (nreverse nodes))
         (web-tldraw-add-node (plist-get n :text)
                              :id (plist-get n :id)
                              :x (plist-get n :x) :y (plist-get n :y)
                              :board board :quiet t))
       (dolist (e (nreverse edges))
         (web-tldraw-add-edge (car e) (cdr e) board))
       (web-tldraw-zoom-fit))
     board)
    board-buf))

(cl-defun web-tldraw-spytial (data spec &key board replace)
  "Lay out DATA under the CnD SPEC on BOARD using spytial-core.
DATA is a Lisp object (serialized to JSON) in JSONDataInstance format
\(:atoms [...] :relations [...]); SPEC is a YAML string of spatial
constraints.  The client computes node positions and builds the board;
the result persists back to the buffer.  With REPLACE non-nil the
board's existing shapes are cleared first (for re-layouts)."
  (web-tldraw--send
   (list :type "tldraw_layout"
         :board (or board web-tldraw-board-id)
         :data data
         :spec (or spec "")
         :replace (if replace t :false))))

;;;; Visualizing Emacs Lisp values (box-and-pointer via spytial)

(defcustom web-tldraw-visualize-max-nodes 300
  "Maximum number of object nodes drawn by `web-tldraw-visualize'."
  :type 'integer :group 'web-tldraw)

(defcustom web-tldraw-visualize-spec nil
  "CnD YAML spec for `web-tldraw-visualize', or nil to auto-generate.
When nil (the default) the spec is built per value from the relations
and types actually present (see `web-tldraw--visualize-spec') — this
matters because spytial errors if a spec references a relation the data
lacks (e.g. `elt' when there are no vectors).  Set a string to override."
  :type '(choice (const :tag "Auto" nil) (string :tag "YAML spec"))
  :group 'web-tldraw)

(defcustom web-tldraw-visualize-group-by-type t
  "When non-nil, `web-tldraw-visualize' groups atoms by type.
Each type with more than one atom becomes a spytial group, drawn as a
labelled dashed container behind its members."
  :type 'boolean :group 'web-tldraw)

(defun web-tldraw--visualize-spec (instance)
  "Build a CnD spec for INSTANCE, referencing only present relations/types.
`car' goes right, `cdr'/`elt' go down (box-and-pointer); each present
atom type is grouped.  No `cyclic' constraint — it conflicts with the
cdr orientation on circular data (the engine then drops to a feasible
subset); cycles still render as back-edges, just not arranged in a ring."
  (let* ((rels (mapcar (lambda (r) (plist-get r :name))
                       (append (plist-get instance :relations) nil)))
         (types (delete-dups
                 (mapcar (lambda (a) (plist-get a :type))
                         (append (plist-get instance :atoms) nil))))
         (dirs '(("car" . "right") ("cdr" . "below") ("elt" . "below")))
         (cons-lines
          (delq nil (mapcar
                     (lambda (r)
                       (let ((d (assoc r dirs)))
                         (when d (format "  - orientation: {selector: %s, directions: [%s]}"
                                         r (cdr d)))))
                     rels)))
         (group-lines
          (when web-tldraw-visualize-group-by-type
            (delq nil (mapcar
                       (lambda (ty)
                         ;; Group only leaf types (the meaningful clusters);
                         ;; grouping Cons/Vector would box the whole
                         ;; structural backbone.  Only types with >1 atom,
                         ;; so single-member boxes don't clutter.
                         (when (and (member ty '("Symbol" "Integer" "String"
                                                 "Float"))
                                    (> (seq-count
                                        (lambda (a) (equal (plist-get a :type) ty))
                                        (append (plist-get instance :atoms) nil))
                                       1))
                           (format "  - group: {selector: %s, name: %s, addEdge: false}"
                                   ty (downcase ty))))
                       types)))))
    ;; NB: `group' must live under `constraints:' (not `directives:') for
    ;; spytial to emit it into layout.groups.
    (when (or cons-lines group-lines)
      (concat "constraints:\n"
              (string-join (append cons-lines group-lines) "\n") "\n"))))

(defun web-tldraw--value-type (obj)
  "Spytial type name for OBJ."
  (cond ((null obj) "Null") ((consp obj) "Cons") ((stringp obj) "String")
        ((symbolp obj) "Symbol") ((integerp obj) "Integer")
        ((floatp obj) "Float") ((recordp obj) "Record")
        ((vectorp obj) "Vector") ((hash-table-p obj) "HashTable")
        (t "Other")))

(defun web-tldraw--value-label (obj)
  "Short display label for OBJ."
  (cond ((null obj) "nil")
        ((consp obj) "cons")
        ((stringp obj) (format "%S" (truncate-string-to-width obj 16 nil nil "…")))
        ((symbolp obj) (truncate-string-to-width (symbol-name obj) 20 nil nil "…"))
        ((numberp obj) (number-to-string obj))
        ((recordp obj) (format "%s" (type-of obj)))
        ((vectorp obj) (format "vec[%d]" (length obj)))
        ((hash-table-p obj) (format "hash{%d}" (hash-table-count obj)))
        (t (truncate-string-to-width (format "%S" obj) 20 nil nil "…"))))

(defun web-tldraw--value->instance (value)
  "Convert VALUE into a spytial JSONDataInstance plist.
Objects are identified by `eq', so shared structure becomes shared
nodes (multiple incoming edges) and cycles become back-edges — which is
exactly what spytial is good at showing."
  (let ((atoms '())
        (rel-tuples (make-hash-table :test 'equal))
        (seen (make-hash-table :test 'eq))
        (n 0) (ell nil))
    (cl-labels
        ((add-rel (name from to)
           (push (list :atoms (vector from to) :types (vector "univ" "univ"))
                 (gethash name rel-tuples)))
         (ellipsis ()
           (or ell (progn (setq ell "ellipsis")
                          (push (list :id ell :type "Other" :label "…") atoms)
                          ell)))
         (walk (obj)
           (let ((hit (gethash obj seen)))
             (cond
              (hit hit)
              ((>= n web-tldraw-visualize-max-nodes) (ellipsis))
              (t (let ((id (format "n%d" (setq n (1+ n)))))
                   (puthash obj id seen)  ; before recursing -> cycle-safe
                   (push (list :id id :type (web-tldraw--value-type obj)
                               :label (web-tldraw--value-label obj))
                         atoms)
                   (cond
                    ((consp obj)
                     (add-rel "car" id (walk (car obj)))
                     (add-rel "cdr" id (walk (cdr obj))))
                    ((and (vectorp obj) (not (recordp obj)))
                     (dotimes (i (min (length obj) 24))
                       (add-rel "elt" id (walk (aref obj i))))))
                   id))))))
      (walk value))
    (let (rels)
      (maphash (lambda (name tuples)
                 (push (list :id name :name name
                             :types (vector "univ" "univ")
                             :tuples (vconcat (nreverse tuples)))
                       rels))
               rel-tuples)
      (list :atoms (vconcat (nreverse atoms))
            :relations (vconcat rels)))))

;;;###autoload
(defun web-tldraw-visualize (value &optional name)
  "Visualize the Emacs Lisp VALUE as a box-and-pointer diagram.
Builds a new board, converts VALUE to a spytial instance (sharing and
cycles preserved via `eq' identity), and lays it out with
`web-tldraw-visualize-spec'.  Interactively, prompts for an expression
to evaluate."
  (interactive
   (list (eval (read--expression "Visualize value: ") lexical-binding)))
  (let* ((buf (web-tldraw-create))
         (board (buffer-local-value 'web-tldraw-board-id buf))
         (data (web-tldraw--value->instance value))
         (spec (or web-tldraw-visualize-spec
                   (web-tldraw--visualize-spec data))))
    (with-current-buffer buf
      (rename-buffer (format "*tldraw-value: %s*" (or name "")) t)
      (setq-local web-tldraw-hide-ui t))
    (web-tldraw-when-ready
     (lambda () (web-tldraw-spytial data spec :board board :replace t))
     board)
    buf))

;;;###autoload
(defun web-tldraw-visualize-demo ()
  "Visualize a value with shared structure (a spytial showcase).
Two cons cells (a . TAIL) and (b . TAIL) share the same TAIL list, so
spytial renders TAIL once with two incoming `cdr' edges — making the
aliasing visible, which a printed representation hides."
  (interactive)
  (let* ((tail (list 'shared 1 2))
         (a (cons 'a tail))
         (b (cons 'b tail))
         (top (list a b)))
    (web-tldraw-visualize top "sharing demo")))

;;;; web-tldraw-dired — a directory tree as an unfoldable board

(defvar-local web-tldraw-dired--root nil
  "Root directory of a `web-tldraw-dired' board.")
(defvar-local web-tldraw-dired--expanded nil
  "List of directory paths currently expanded in the dired board.")
(defvar-local web-tldraw-dired--path->shape nil
  "Hash mapping file paths to their current shape id in the dired board.")

(defcustom web-tldraw-dired-layer-width 280
  "Horizontal pixels between tree depth levels in a dired board.
Larger values spread the layers further apart."
  :type 'integer :group 'web-tldraw)

(defcustom web-tldraw-dired-row-height 60
  "Vertical pixels per entry (row) in a dired board."
  :type 'integer :group 'web-tldraw)

(defcustom web-tldraw-dired-box-width 230
  "Width in pixels of each entry box in a dired board."
  :type 'integer :group 'web-tldraw)

(defcustom web-tldraw-dired-name-max 26
  "Maximum displayed name length in a dired board (longer names are
truncated with an ellipsis so the label stays on one line and boxes do
not overlap).  The full path is still available to actions."
  :type 'integer :group 'web-tldraw)

(defcustom web-tldraw-dired-draw-edges nil
  "Whether to draw parent->child arrows in a dired board.
Off by default: the indented columns convey the hierarchy more cleanly
than arrows, which clutter densely-populated trees."
  :type 'boolean :group 'web-tldraw)

(defcustom web-tldraw-dired-find-file-function #'find-file-other-window
  "How RET opens a file from a dired board.
Called with the file's path.  `find-file-other-window' keeps the board
visible; `find-file' replaces the board's window."
  :type '(choice (const :tag "Other window" find-file-other-window)
                 (const :tag "Same window" find-file)
                 (function :tag "Custom function"))
  :group 'web-tldraw)

(defcustom web-tldraw-dired-icon-function #'web-tldraw-dired-default-icon
  "Function returning a node's icon string, called (PATH KIND EXPANDED).
KIND is `dir' or `file'; EXPANDED is non-nil for an open directory.  The
default returns Unicode glyphs by file type.  Set this to integrate
e.g. all-the-icons (return a glyph the board font can render)."
  :type 'function :group 'web-tldraw)

(defun web-tldraw-dired-default-icon (path kind expanded)
  "Default Unicode icon for PATH of KIND (dir/file), EXPANDED dirs open."
  (cond
   ((eq kind 'dir) (if expanded "📂" "📁"))
   (t (pcase (downcase (or (file-name-extension path) ""))
        ((or "el" "elc") "λ")
        ((or "c" "h" "cpp" "cc" "hpp") "🔧")
        ((or "js" "jsx" "ts" "tsx" "mjs") "🟨")
        ("py" "🐍")
        ((or "md" "org" "rst" "txt" "text") "📝")
        ((or "png" "jpg" "jpeg" "gif" "svg" "webp" "bmp" "ico") "🖼")
        ((or "json" "yaml" "yml" "toml" "ini" "cfg" "conf") "⚙")
        ((or "sh" "bash" "zsh" "fish") "❯")
        ((or "zip" "gz" "tar" "xz" "bz2" "zst" "7z") "📦")
        ((or "pdf") "📕")
        ((or "lock") "🔒")
        ("" (if (string-prefix-p "." (file-name-nondirectory path)) "⚙" "📄"))
        (_ "📄")))))

(defun web-tldraw-dired--visible ()
  "Return visible entries as (PATH KIND PARENT-PATH DEPTH) in preorder."
  (let ((nodes '()))
    (cl-labels
        ((walk (path parent depth)
           (let ((dirp (file-directory-p path)))
             (push (list path (if dirp 'dir 'file) parent depth) nodes)
             (when (and dirp (member path web-tldraw-dired--expanded))
               (dolist (child (ignore-errors
                                (directory-files
                                 path t directory-files-no-dot-files-regexp)))
                 (walk child path (1+ depth)))))))
      (walk web-tldraw-dired--root nil 0))
    (nreverse nodes)))

(defun web-tldraw-dired--rebuild (&optional focus-path)
  "Lay out the visible tree as an indented outline (clearing first).
Each entry sits at x = depth * `web-tldraw-dired-layer-width', y =
preorder-row * `web-tldraw-dired-row-height'.  FOCUS-PATH, if given, is
re-selected after the rebuild (so unfolding keeps your place)."
  (clrhash web-tldraw-dired--path->shape)
  (let* ((nodes (web-tldraw-dired--visible))
         (board web-tldraw-board-id)
         (lw web-tldraw-dired-layer-width)
         (rh web-tldraw-dired-row-height)
         (row 0))
    (web-tldraw--cmd "clear" nil board)
    (dolist (n nodes)
      (let* ((path (nth 0 n)) (kind (nth 1 n)) (depth (nth 3 n))
             (dirp (eq kind 'dir))
             (raw (file-name-nondirectory (directory-file-name path)))
             ;; Truncate to one line so boxes never grow and overlap.
             (name (truncate-string-to-width
                    raw web-tldraw-dired-name-max nil nil "…"))
             (expanded (and dirp (member path web-tldraw-dired--expanded)))
             (icon (funcall web-tldraw-dired-icon-function path kind expanded))
             (label (format "%s %s%s" icon name (if dirp "/" "")))
             (nx (* depth lw)) (ny (* row rh))
             (sid (format "shape:dired%d" row)))
        (puthash path sid web-tldraw-dired--path->shape)
        (web-tldraw-add-node label
                             :id sid
                             :x nx :y ny
                             :w web-tldraw-dired-box-width :h 44
                             :size "s"
                             :board board :quiet t
                             :meta (list :path path :kind (symbol-name kind)
                                         :x nx :y ny))
        (setq row (1+ row))))
    (when web-tldraw-dired-draw-edges
      (dolist (n nodes)
        (when (nth 2 n)
          (let ((ps (gethash (nth 2 n) web-tldraw-dired--path->shape))
                (cs (gethash (nth 0 n) web-tldraw-dired--path->shape)))
            (when (and ps cs) (web-tldraw-add-edge ps cs board))))))
    ;; Always seed a selection so navigation has a starting point and
    ;; RET acts on the node you are actually looking at.  Re-select the
    ;; toggled node after an unfold, else select the root.
    (let ((sid (gethash (or focus-path web-tldraw-dired--root)
                        web-tldraw-dired--path->shape)))
      (when sid (web-tldraw-select (list sid) board)))
    (unless focus-path (web-tldraw-zoom-fit))))

(defun web-tldraw-dired--action (board _id meta _text)
  "Node-action hook (RET) for dired boards.
Unfold directories, embed image files on the canvas, open other files."
  (when (and web-tldraw-dired--root meta)
    (let ((path (alist-get 'path meta))
          (kind (alist-get 'kind meta))
          (nx (alist-get 'x meta))
          (ny (alist-get 'y meta)))
      (when path
        (cond
         ((or (equal kind "dir") (file-directory-p path))
          (if (member path web-tldraw-dired--expanded)
              (setq web-tldraw-dired--expanded
                    (delete path web-tldraw-dired--expanded))
            (push path web-tldraw-dired--expanded))
          (web-tldraw-dired--rebuild path)
          t)
         ((web-tldraw-image-file-p path)
          ;; Embed the image to the right of the tree, level with its row.
          (web-tldraw-add-image path
                                :x (+ (or nx 0) web-tldraw-dired-layer-width 60)
                                :y (or ny 0)
                                :board board)
          t)
         (t (funcall web-tldraw-dired-find-file-function path) t))))))

;;;###autoload
(defun web-tldraw-dired (dir)
  "Open DIR as an unfoldable directory-tree whiteboard.
Entries are laid out as an indented outline tree (depth -> column,
preorder -> row).  RET on a directory unfolds/folds it; RET on a file
opens it.  The mouse UI is hidden (keyboard-first); navigate with the
`tldraw-mode' keys, `m' for mouse mode."
  (interactive "DDirectory: ")
  (let* ((dir (expand-file-name dir))
         (buf (web-tldraw-create)))
    (with-current-buffer buf
      (rename-buffer (format "*tldraw-dired: %s*"
                             (abbreviate-file-name dir))
                     t)
      (setq web-tldraw-dired--root dir
            web-tldraw-dired--expanded (list dir)
            web-tldraw-dired--path->shape (make-hash-table :test 'equal))
      (setq-local web-tldraw-hide-ui t)  ; keyboard-first; `m' reveals UI
      (add-hook 'web-tldraw-node-action-functions
                #'web-tldraw-dired--action nil t)
      (web-tldraw-when-ready #'web-tldraw-dired--rebuild))
    buf))

;; Re-theme all boards when the Emacs theme changes (Emacs 29+).
(when (boundp 'enable-theme-functions)
  (add-hook 'enable-theme-functions #'web-tldraw-sync-theme))
(when (boundp 'disable-theme-functions)
  (add-hook 'disable-theme-functions #'web-tldraw-sync-theme))

(provide 'web-tldraw)

;;; web-tldraw.el ends here
